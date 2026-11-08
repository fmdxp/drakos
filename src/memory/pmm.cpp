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


#include "pmm.hpp"
#include "limine_requests.hpp"

// -------------------------------------------------------------------
// INTERNAL STATE
// All variables here are file-local (static). They are accessed only
// through the public API functions declared in pmm.hpp.
// -------------------------------------------------------------------

// The HHDM offset from Limine. Add this to any physical address to
// get the virtual address we can actually read/write from C++.
// Example: uint8_t* ptr = (uint8_t*)(some_phys_addr + s_hhdm_offset);
static uint64_t s_hhdm_offset = 0;

// Total number of 4KB pages in the system
static uint64_t s_total_pages = 0;

// -------------------------------------------------------------------
// BUDDY SYSTEM FREE LISTS
//
// The buddy system manages blocks of size 2^N pages (N = order).
// For each order, we keep a linked list of free blocks.
//
// Key insight: instead of a separate metadata array, we store the
// "next" pointer INSIDE the free block itself (at its physical address,
// accessed via HHDM). This is called an "intrusive linked list".
// A free block always has at least PAGE_SIZE bytes, so there's always
// room for one pointer.
// -------------------------------------------------------------------

struct FreeBlock {
    FreeBlock* next;
};

// One free list head per order (0 = 4KB blocks, MAX_ORDER = 4MB blocks)
static FreeBlock* s_free_lists[MAX_ORDER + 1];

// -------------------------------------------------------------------
// PAGE STATE TRACKING
//
// For each physical page, we store:
//   0xFF      = interior page or allocated page without head metadata
//   0 - 10    = this page is the START of a free block of that order
//   0x80-0x8A = allocated block head, with the order encoded in the value
//
// This lets us detect the buddy in O(1):
//   - Compute buddy address: buddy = addr XOR (PAGE_SIZE << order)
//   - Check if s_page_order[buddy >> PAGE_SHIFT] == order
//   - If yes, the buddy is free at the same order → we can merge!
// -------------------------------------------------------------------

#define PAGE_ALLOCATED 0xFF
#define PAGE_ALLOCATED_HEAD 0x80
static uint8_t* s_page_order = nullptr;  // Array of s_total_pages entries
static volatile bool s_pmm_lock = false;

static inline uint64_t lock_pmm() {
#ifdef DRAKOS_HOST_TEST
    return 0;
#else
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&s_pmm_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
    return flags;
#endif
}

static inline void unlock_pmm(uint64_t flags) {
#ifndef DRAKOS_HOST_TEST
    __atomic_clear(&s_pmm_lock, __ATOMIC_RELEASE);
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

static bool map_entry_end(const limine_memmap_entry* entry, uint64_t& end) {
    if (entry->length > UINT64_MAX - entry->base) return false;
    end = entry->base + entry->length;
    return true;
}

static bool align_up_page(uint64_t value, uint64_t& aligned) {
    if (value > UINT64_MAX - (PAGE_SIZE - 1)) return false;
    aligned = (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return true;
}

// -------------------------------------------------------------------
// INTERNAL HELPERS
// -------------------------------------------------------------------

// Convert a physical address to a FreeBlock pointer (via HHDM)
static inline FreeBlock* phys_to_block(uintptr_t phys) {
    return reinterpret_cast<FreeBlock*>(phys + s_hhdm_offset);
}

// Convert a FreeBlock pointer back to its physical address
static inline uintptr_t block_to_phys(FreeBlock* block) {
    return reinterpret_cast<uintptr_t>(block) - s_hhdm_offset;
}

// Push a block onto the free list for the given order
static void free_list_push(uint32_t order, uintptr_t phys) {
    FreeBlock* block  = phys_to_block(phys);
    block->next       = s_free_lists[order];
    s_free_lists[order] = block;
}

// Pop the first block from the free list for the given order.
// Returns 0 if the list is empty.
static uintptr_t free_list_pop(uint32_t order) {
    if (!s_free_lists[order]) return 0;
    FreeBlock* block    = s_free_lists[order];
    s_free_lists[order] = block->next;
    return block_to_phys(block);
}

// Remove a specific block from the free list (needed for buddy merging).
// Returns true if found and removed.
static bool free_list_remove(uint32_t order, uintptr_t phys) {
    FreeBlock** curr = &s_free_lists[order];
    while (*curr) {
        if (block_to_phys(*curr) == phys) {
            *curr = (*curr)->next;
            return true;
        }
        curr = &(*curr)->next;
    }
    return false;
}

// -------------------------------------------------------------------
// PUBLIC API IMPLEMENTATION
// -------------------------------------------------------------------

uintptr_t pmm_alloc(uint32_t order) {
    uint64_t flags = lock_pmm();
    if (!s_page_order || order > MAX_ORDER) {
        unlock_pmm(flags);
        return 0;
    }

    uint32_t found_order = order;
    
    // Step 1: Find the first order that has at least one free block
    while (found_order <= MAX_ORDER) {
        if (s_free_lists[found_order] != nullptr) {
            break; // Found one!
        }
        found_order++;
    }

    // Step 2: If we exceeded MAX_ORDER, we are out of memory
    if (found_order > MAX_ORDER) {
        unlock_pmm(flags);
        return 0; // OOM (Out Of Memory)
    }

    // Step 3: Remove the block from its current list
    uintptr_t phys = free_list_pop(found_order);

    // Step 4: Mark the block as allocated in our state array
    s_page_order[phys >> PAGE_SHIFT] = PAGE_ALLOCATED_HEAD + order;

    // Step 5: Split the block if we found one larger than requested
    while (found_order > order) {
        found_order--;
        
        // Calculate the address of the "second half" (the buddy)
        uintptr_t buddy = phys + (PAGE_SIZE << found_order);
        
        // Add the buddy to the free list of the new smaller order
        free_list_push(found_order, buddy);
        
        // Mark the buddy as free at this new order
        s_page_order[buddy >> PAGE_SHIFT] = found_order;
    }

    // Step 6: Return the physical address of our allocated block
    unlock_pmm(flags);
    return phys;
}

void pmm_free(uintptr_t phys_addr, uint32_t order) {
    uint64_t flags = lock_pmm();
    if (!s_page_order || phys_addr == 0 || order > MAX_ORDER) {
        unlock_pmm(flags);
        return;
    }

    uintptr_t block_size = PAGE_SIZE << order;
    uint64_t page = phys_addr >> PAGE_SHIFT;
    uint64_t page_count = 1ULL << order;
    if ((phys_addr & (block_size - 1)) != 0 || page >= s_total_pages ||
        page_count > s_total_pages - page ||
        s_page_order[page] != PAGE_ALLOCATED_HEAD + order) {
        unlock_pmm(flags);
        return;
    }

    uintptr_t phys = phys_addr;
    
    // Step 1: Attempt to merge with buddy until we reach MAX_ORDER
    while (order < MAX_ORDER) {
        // Calculate the buddy's physical address (XOR magic)
        uintptr_t buddy = phys ^ (PAGE_SIZE << order);
        uint64_t buddy_page = buddy >> PAGE_SHIFT;
        
        // Ensure the buddy is within physical RAM limits
        if (buddy_page >= s_total_pages) {
            break;
        }
        
        // Ensure the buddy is actually free and of the exact same order
        if (s_page_order[buddy_page] != order) {
            break;
        }
        
        // The buddy is available! Let's merge.
        // Remove buddy from its current free list
        if (!free_list_remove(order, buddy)) break;
        
        // Mark the buddy as allocated (it's no longer a standalone free block)
        s_page_order[buddy_page] = PAGE_ALLOCATED;
        
        // The newly merged block always starts at the lower of the two addresses
        if (buddy < phys) {
            phys = buddy;
        }
        
        // Increase the order for the next iteration
        order++;
    }
    
    // Step 2: Add the final (possibly merged) block to the free list
    free_list_push(order, phys);
    
    // Step 3: Mark this block as free in our state array
    const uint64_t merged_pages = 1ULL << order;
    for (uint64_t i = 0; i < merged_pages; ++i)
        s_page_order[(phys >> PAGE_SHIFT) + i] = PAGE_ALLOCATED;
    s_page_order[phys >> PAGE_SHIFT] = order;
    unlock_pmm(flags);
}

uint64_t pmm_hhdm_offset() {
    return s_hhdm_offset;
}

bool pmm_is_allocated_page(uintptr_t phys_addr) {
    uint64_t flags = lock_pmm();
    bool allocated = false;
    if (s_page_order && phys_addr != 0 && (phys_addr & (PAGE_SIZE - 1)) == 0) {
        uint64_t page = phys_addr >> PAGE_SHIFT;
        allocated = page < s_total_pages && s_page_order[page] == PAGE_ALLOCATED_HEAD;
    }
    unlock_pmm(flags);
    return allocated;
}

// -------------------------------------------------------------------
// MODULE LIFECYCLE
// -------------------------------------------------------------------

bool PMM::start() {
    s_page_order = nullptr;
    s_total_pages = 0;
    s_hhdm_offset = 0;
    for (uint32_t order = 0; order <= MAX_ORDER; ++order) s_free_lists[order] = nullptr;

    // 1. Read HHDM offset
    if (!g_hhdm_request.response) return false;
    s_hhdm_offset = g_hhdm_request.response->offset;

    // 2. Read physical memory map
    if (!g_memmap_request.response) return false;
    if (g_memmap_request.response->entry_count != 0 && !g_memmap_request.response->entries) return false;

    // 3. Find the highest usable address to size page metadata.
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < g_memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* entry = g_memmap_request.response->entries[i];
        if (!entry) return false;
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length != 0) {
            uint64_t top;
            if (!map_entry_end(entry, top)) return false;
            if (top > highest_addr) highest_addr = top;
        }
    }
    s_total_pages = highest_addr / PAGE_SIZE + (highest_addr % PAGE_SIZE != 0);

    // If there is no usable RAM, the kernel cannot function
    if (s_total_pages == 0) return false;

    // 4. The s_page_order array takes 1 byte per page. Find space for it in RAM.
    if (s_total_pages > UINT64_MAX - (PAGE_SIZE - 1)) return false;
    uint64_t state_array_size = s_total_pages;
    // MUST align to PAGE_SIZE to keep buddy allocator blocks aligned!
    state_array_size = (state_array_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    bool metadata_reserved = false;
    for (uint64_t i = 0; i < g_memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* entry = g_memmap_request.response->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length != 0) {
            uint64_t end;
            uint64_t start;
            if (!map_entry_end(entry, end) ||
                !align_up_page(entry->base < PAGE_SIZE ? PAGE_SIZE : entry->base, start) ||
                start > end || end - start < state_array_size) continue;

            // Keep page zero reserved; zero is also the allocation failure value.
            s_page_order = reinterpret_cast<uint8_t*>(start + s_hhdm_offset);
            entry->base = start + state_array_size;
            entry->length = end - entry->base;
            metadata_reserved = true;
            break;
        }
    }

    if (!metadata_reserved) return false; // Not enough RAM for the state array!

    // 5. Initialize everything as allocated (0xFF)
    for (uint64_t i = 0; i < s_total_pages; i++) {
        s_page_order[i] = PAGE_ALLOCATED;
    }

    // 7. Fill the Buddy System with the remaining RAM
    for (uint64_t i = 0; i < g_memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry* entry = g_memmap_request.response->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length == 0) continue;

        uint64_t end;
        uint64_t addr;
        if (!map_entry_end(entry, end) ||
            !align_up_page(entry->base < PAGE_SIZE ? PAGE_SIZE : entry->base, addr)) return false;
        end &= ~(PAGE_SIZE - 1);
        if (addr >= end) continue;
        uint64_t len = end - addr;

        // Split this space into the largest possible blocks
        while (len >= PAGE_SIZE) {
            // What is the maximum order that fits in 'len'?
            uint32_t order = MAX_ORDER;
            while (order > 0) {
                uint64_t block_size = PAGE_SIZE << order;
                // It must fit, AND the physical address must be naturally aligned
                if (len >= block_size && (addr & (block_size - 1)) == 0) {
                    break;
                }
                order--;
            }

            // Add the block to the list
            free_list_push(order, addr);
            s_page_order[addr >> PAGE_SHIFT] = order;

            // Move forward
            uint64_t used_size = PAGE_SIZE << order;
            addr += used_size;
            len  -= used_size;
        }
    }

    return true;
}

void PMM::stop() {
    // The PMM cannot be stopped once running.
    // Physical memory management is fundamental to the OS.
}

const char* PMM::get_name() const {
    return "Physical Memory Manager (Buddy System)";
}

// Register as the FIRST memory module
REGISTER_MODULE_REQUIRED_NO_DEPS(g_pmm, PMM, 2_mem_a);
