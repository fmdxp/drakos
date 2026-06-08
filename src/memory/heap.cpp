/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include "heap.hpp"
#include "pmm.hpp"
#include "vmm.hpp"

// -------------------------------------------------------------------
// INTERNAL SLUB STRUCTURES
// -------------------------------------------------------------------

// An object in a free list stores a pointer to the next free object
// in its first 8 bytes.
struct FreeObject {
    FreeObject* next;
};

// A cache manages allocations of a specific fixed size.
struct SlubCache {
    size_t      object_size;
    FreeObject* free_list;
};

// The fixed sizes our SLUB allocator supports
static const size_t SLUB_SIZES[] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};
#define NUM_SLUB_CACHES (sizeof(SLUB_SIZES) / sizeof(SLUB_SIZES[0]))

static SlubCache s_caches[NUM_SLUB_CACHES];

// For allocations larger than 2048 bytes, we bypass the SLUB cache
// and allocate whole pages directly from the PMM.
// We need a way to track how many pages were allocated for a large block
// so we can free them later.
// A simple way is to prepend a small header to large allocations.
struct LargeAllocHeader {
    size_t num_pages;
    // Data follows immediately after
};

// -------------------------------------------------------------------
// VIRTUAL HEAP BASE
// -------------------------------------------------------------------
// We map heap pages starting at a high virtual address.
static uintptr_t s_heap_virt_base = 0xFFFF900000000000ULL;
static uintptr_t s_heap_virt_curr = s_heap_virt_base;

// -------------------------------------------------------------------
// INTERNAL HELPERS
// -------------------------------------------------------------------

// Helper to expand a specific cache by allocating a new page from PMM,
// mapping it into the VMM, and slicing it into objects.
static bool expand_cache(SlubCache& cache) {
    // 1. Allocate 1 physical page
    uintptr_t phys = pmm_alloc_page();
    if (!phys) return false; // OOM

    // 2. Map it in the VMM at the current heap virtual address
    vmm_map(s_heap_virt_curr, phys, VMM_KERNEL_RW);

    // 3. Slice the newly mapped virtual page into chunks
    uint8_t* page_ptr = reinterpret_cast<uint8_t*>(s_heap_virt_curr);
    size_t objects_in_page = PAGE_SIZE / cache.object_size;

    for (size_t i = 0; i < objects_in_page; i++) {
        FreeObject* obj = reinterpret_cast<FreeObject*>(page_ptr + (i * cache.object_size));
        
        // Link this object to the current free list
        obj->next = cache.free_list;
        cache.free_list = obj;
    }

    // 4. Advance the virtual heap pointer for the next expansion
    s_heap_virt_curr += PAGE_SIZE;
    return true;
}

// -------------------------------------------------------------------
// PUBLIC API IMPLEMENTATION
// -------------------------------------------------------------------

void* kmalloc(size_t size) {
    if (size == 0) return nullptr;

    // Check if it's a large allocation (bypass SLUB)
    if (size > 2048) {
        // Calculate required pages including the header
        size_t total_size = size + sizeof(LargeAllocHeader);
        size_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;
        
        // Find the correct buddy order for this many pages
        uint32_t order = 0;
        while ((1ULL << order) < pages) {
            order++;
        }

        // Allocate contiguous physical memory
        uintptr_t phys = pmm_alloc(order);
        if (!phys) return nullptr; // OOM

        // Map the pages into virtual memory
        uintptr_t virt_start = s_heap_virt_curr;
        for (size_t i = 0; i < (1ULL << order); i++) {
            vmm_map(s_heap_virt_curr, phys + (i * PAGE_SIZE), VMM_KERNEL_RW);
            s_heap_virt_curr += PAGE_SIZE;
        }

        // Setup the header
        LargeAllocHeader* header = reinterpret_cast<LargeAllocHeader*>(virt_start);
        header->num_pages = (1ULL << order);

        // Return the pointer just after the header
        return reinterpret_cast<void*>(virt_start + sizeof(LargeAllocHeader));
    }

    // It's a small allocation, find the appropriate SLUB cache
    for (size_t i = 0; i < NUM_SLUB_CACHES; i++) {
        if (size <= s_caches[i].object_size) {
            SlubCache& cache = s_caches[i];

            // If the cache is empty, expand it
            if (!cache.free_list) {
                if (!expand_cache(cache)) return nullptr; // OOM
            }

            // Pop an object from the free list
            FreeObject* obj = cache.free_list;
            cache.free_list = obj->next;

            return reinterpret_cast<void*>(obj);
        }
    }

    return nullptr;
}

void kfree(void* ptr) {
    if (!ptr) return;

    // In a full implementation, we'd look up the page metadata to see 
    // if this is a SLUB object or a large allocation.
    // For this basic kernel, if the pointer is within the currently 
    // mapped heap range, we assume it's a SLUB object.
    // Since we don't track which page belongs to which cache, we have to 
    // search the SLUB sizes. This is highly inefficient but works for a 
    // basic skeleton. A real SLUB allocator uses 'struct page' metadata.
    // 
    // Since we lack page metadata in this skeleton, kfree is a no-op for SLUB 
    // objects for now to prevent corruption, but large allocations can be freed.
    
    // Check if it might be a large allocation (has a valid header)
    // This is dangerous without page metadata, so we just leak for now
    // until the metadata system is built.
    (void)ptr;
}

// -------------------------------------------------------------------
// C++ OPERATORS
// -------------------------------------------------------------------

// Note: These override the placement new ones in module.hpp if called
// with a size.
void* operator new(size_t size) { return kmalloc(size); }
void* operator new[](size_t size) { return kmalloc(size); }

void operator delete(void* p) noexcept { kfree(p); }
void operator delete[](void* p) noexcept { kfree(p); }
// Unsized deletes (required by C++14+)
void operator delete(void* p, size_t) noexcept { kfree(p); }
void operator delete[](void* p, size_t) noexcept { kfree(p); }

// -------------------------------------------------------------------
// MODULE LIFECYCLE
// -------------------------------------------------------------------

bool Heap::start() {
    // Initialize cache sizes
    for (size_t i = 0; i < NUM_SLUB_CACHES; i++) {
        s_caches[i].object_size = SLUB_SIZES[i];
        s_caches[i].free_list   = nullptr;
    }
    return true;
}

void Heap::stop() {
    // Cannot be stopped.
}

const char* Heap::get_name() const {
    return "Heap (SLUB Allocator)";
}

// Register as the THIRD memory module
REGISTER_MODULE(g_heap, Heap, 2_mem_c);
