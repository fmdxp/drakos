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

#include <stdint.h>

namespace std { enum class align_val_t : size_t {}; }

struct FreeObject { FreeObject* next; };
struct SlubCache { size_t object_size; FreeObject* free_list; };
static const size_t SLUB_SIZES[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048};
#define NUM_SLUB_CACHES (sizeof(SLUB_SIZES) / sizeof(SLUB_SIZES[0]))
static SlubCache s_caches[NUM_SLUB_CACHES];

static constexpr uintptr_t HEAP_VIRT_BASE = 0xFFFF900000000000ULL;
static uintptr_t s_heap_virt_curr = HEAP_VIRT_BASE;
static volatile bool s_heap_lock = false;
static constexpr uint64_t SLAB_MAGIC = 0x534C5542534C4142ULL;
static constexpr uint64_t LARGE_MAGIC = 0x4C41524745414C4CULL;
static constexpr uint64_t FREED_LARGE_MAGIC = 0x46524545444C4152ULL;
static constexpr size_t SLAB_BITMAP_WORDS = 8;

struct SlabPage {
    uint64_t magic;
    uintptr_t virtual_base;
    uint16_t cache_index;
    uint16_t slot_count;
    uint16_t allocated_count;
    uint16_t reserved;
    uint64_t allocated[SLAB_BITMAP_WORDS];
};
static constexpr size_t SLAB_DATA_OFFSET = (sizeof(SlabPage) + 15) & ~size_t(15);
static_assert(SLAB_DATA_OFFSET < PAGE_SIZE);
static_assert((PAGE_SIZE - SLAB_DATA_OFFSET) / 8 <= SLAB_BITMAP_WORDS * 64);

struct LargeAllocHeader {
    uint64_t magic;
    uintptr_t base_virtual;
    uintptr_t physical_base;
    uintptr_t user_pointer;
    uint32_t order;
    uint32_t reserved;
    size_t requested_size;
    uint8_t padding[16];
};
static_assert(sizeof(LargeAllocHeader) <= 64);

#ifdef DRAKOS_HOST_TEST
extern uintptr_t g_heap_test_virtual_base;
#endif

static uint64_t lock_heap() {
#ifdef DRAKOS_HOST_TEST
    while (__atomic_test_and_set(&s_heap_lock, __ATOMIC_ACQUIRE)) {}
    return 0;
#else
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&s_heap_lock, __ATOMIC_ACQUIRE)) asm volatile("pause");
    return flags;
#endif
}

static void unlock_heap(uint64_t flags) {
    __atomic_clear(&s_heap_lock, __ATOMIC_RELEASE);
#ifndef DRAKOS_HOST_TEST
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

static uintptr_t align_up(uintptr_t value, size_t alignment) {
    return (value + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
}
static bool is_power_of_two(size_t value) { return value && !(value & (value - 1)); }

static uintptr_t heap_base() {
#ifdef DRAKOS_HOST_TEST
    return g_heap_test_virtual_base;
#else
    return HEAP_VIRT_BASE;
#endif
}

static bool mapped_heap_address(uintptr_t address, VmmMappingInfo* info) {
    if (address < heap_base() || address >= s_heap_virt_curr) return false;
    if (!vmm_query_mapping(address, info)) return false;
    return info->ownership == VmmMappingInfo::Ownership::Owned ||
           info->ownership == VmmMappingInfo::Ownership::Borrowed;
}

static size_t slot_count_for(size_t cache_index) {
    return (PAGE_SIZE - SLAB_DATA_OFFSET) / s_caches[cache_index].object_size;
}

static bool slab_slot_index(const SlabPage* slab, uintptr_t pointer, size_t& index) {
    if (slab->magic != SLAB_MAGIC || slab->cache_index >= NUM_SLUB_CACHES ||
        pointer < slab->virtual_base + SLAB_DATA_OFFSET) return false;
    size_t object_size = s_caches[slab->cache_index].object_size;
    uintptr_t offset = pointer - (slab->virtual_base + SLAB_DATA_OFFSET);
    if ((offset % object_size) != 0) return false;
    index = offset / object_size;
    return index < slab->slot_count;
}

static bool slot_is_allocated(const SlabPage* slab, size_t index) {
    return (slab->allocated[index / 64] & (1ULL << (index % 64))) != 0;
}
static void set_slot_allocated(SlabPage* slab, size_t index) {
    slab->allocated[index / 64] |= 1ULL << (index % 64);
}
static void clear_slot_allocated(SlabPage* slab, size_t index) {
    slab->allocated[index / 64] &= ~(1ULL << (index % 64));
}

static void build_slab_free_list(SlabPage* slab, SlubCache& cache) {
    for (size_t i = 0; i < slab->slot_count; ++i) {
        auto* object = reinterpret_cast<FreeObject*>(slab->virtual_base + SLAB_DATA_OFFSET + i * cache.object_size);
        object->next = cache.free_list;
        cache.free_list = object;
    }
}

static bool expand_cache(size_t cache_index) {
    SlubCache& cache = s_caches[cache_index];
    uintptr_t physical = pmm_alloc_page();
    if (!physical) return false;
    uintptr_t virtual_base = s_heap_virt_curr;
    if (virtual_base > UINTPTR_MAX - PAGE_SIZE ||
        vmm_map_owned_page(vmm_get_kernel_pml4(), virtual_base, physical, VMM_KERNEL_RW) != VmmMapResult::Success) {
        pmm_free_page(physical);
        return false;
    }
    auto* slab = reinterpret_cast<SlabPage*>(virtual_base);
    for (size_t i = 0; i < sizeof(SlabPage); ++i) reinterpret_cast<uint8_t*>(slab)[i] = 0;
    slab->magic = SLAB_MAGIC;
    slab->virtual_base = virtual_base;
    slab->cache_index = static_cast<uint16_t>(cache_index);
    slab->slot_count = static_cast<uint16_t>(slot_count_for(cache_index));
    if (!slab->slot_count) {
        vmm_unmap_page(vmm_get_kernel_pml4(), virtual_base);
        return false;
    }
    build_slab_free_list(slab, cache);
    s_heap_virt_curr += PAGE_SIZE;
    return true;
}

static void unlink_slab_free_objects(SlubCache& cache, uintptr_t base) {
    uintptr_t end = base + PAGE_SIZE;
    FreeObject** link = &cache.free_list;
    while (*link) {
        FreeObject* object = *link;
        uintptr_t address = reinterpret_cast<uintptr_t>(object);
        if (address >= base && address < end) *link = object->next;
        else link = &object->next;
    }
}

static void reclaim_empty_slab(SlabPage* slab, SlubCache& cache) {
    uintptr_t base = slab->virtual_base;
    unlink_slab_free_objects(cache, base);
    slab->magic = 0;
    VmmUnmapResult result = vmm_unmap_page(vmm_get_kernel_pml4(), base);
    if (result != VmmUnmapResult::Success) {
        slab->magic = SLAB_MAGIC;
        build_slab_free_list(slab, cache);
    }
}

static void* allocate_large(size_t size, size_t alignment) {
    if (!is_power_of_two(alignment) || alignment > (PAGE_SIZE << (MAX_ORDER - 1))) return nullptr;
    if (size > SIZE_MAX - sizeof(LargeAllocHeader) - sizeof(uintptr_t) - alignment) return nullptr;
    size_t required = size + sizeof(LargeAllocHeader) + sizeof(uintptr_t) + alignment - 1;
    if (required > SIZE_MAX - (PAGE_SIZE - 1)) return nullptr;
    size_t pages = (required + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t order = 0;
    while ((size_t(1) << order) < pages && order <= MAX_ORDER) ++order;
    if (order > MAX_ORDER) return nullptr;
    size_t block_pages = size_t(1) << order;
    if (block_pages > SIZE_MAX / PAGE_SIZE || s_heap_virt_curr > UINTPTR_MAX - block_pages * PAGE_SIZE) return nullptr;

    uintptr_t physical = pmm_alloc(order);
    if (!physical) return nullptr;
    uintptr_t base = s_heap_virt_curr;
    uintptr_t root = vmm_get_kernel_pml4();
    size_t mapped = 0;
    for (; mapped < block_pages; ++mapped) {
        if (vmm_map_page(root, base + mapped * PAGE_SIZE, physical + mapped * PAGE_SIZE,
                         VMM_KERNEL_RW) != VmmMapResult::Success) break;
    }
    if (mapped != block_pages) {
        while (mapped) vmm_unmap_page(root, base + --mapped * PAGE_SIZE);
        pmm_free(physical, order);
        return nullptr;
    }

    auto* header = reinterpret_cast<LargeAllocHeader*>(base);
    header->magic = LARGE_MAGIC;
    header->base_virtual = base;
    header->physical_base = physical;
    header->order = order;
    header->requested_size = size;
    uintptr_t payload = align_up(base + sizeof(LargeAllocHeader) + sizeof(uintptr_t), alignment);
    header->user_pointer = payload;
    *reinterpret_cast<LargeAllocHeader**>(payload - sizeof(LargeAllocHeader*)) = header;
    s_heap_virt_curr += block_pages * PAGE_SIZE;
    return reinterpret_cast<void*>(payload);
}

void* kmalloc(size_t size) {
    if (!size) return nullptr;
    uint64_t flags = lock_heap();
    if (size > 2048) {
        void* result = allocate_large(size, 16);
        unlock_heap(flags);
        return result;
    }

    size_t requested = size < 16 ? 16 : size;
    size_t cache_index = 0;
    while (cache_index < NUM_SLUB_CACHES && requested > s_caches[cache_index].object_size)
        ++cache_index;
    if (cache_index == NUM_SLUB_CACHES) { unlock_heap(flags); return nullptr; }
    if (!s_caches[cache_index].free_list && !expand_cache(cache_index)) {
        unlock_heap(flags);
        return nullptr;
    }

    FreeObject* object = s_caches[cache_index].free_list;
    s_caches[cache_index].free_list = object->next;
    uintptr_t base = reinterpret_cast<uintptr_t>(object) & ~(PAGE_SIZE - 1);
    auto* slab = reinterpret_cast<SlabPage*>(base);
    size_t index = 0;
    if (slab->cache_index != cache_index || !slab_slot_index(slab, reinterpret_cast<uintptr_t>(object), index) ||
        slot_is_allocated(slab, index)) {
        unlock_heap(flags);
        return nullptr;
    }
    set_slot_allocated(slab, index);
    ++slab->allocated_count;
    unlock_heap(flags);
    return object;
}

void* kmalloc_aligned(size_t size, size_t alignment) {
    if (!size || !is_power_of_two(alignment)) return nullptr;
    if (alignment <= 16) return kmalloc(size);
    uint64_t flags = lock_heap();
    void* result = allocate_large(size, alignment);
    unlock_heap(flags);
    return result;
}

void kfree(void* pointer) {
    if (!pointer) return;
    uint64_t flags = lock_heap();
    uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (address < heap_base() + sizeof(uintptr_t) || address >= s_heap_virt_curr) {
        unlock_heap(flags);
        return;
    }
    VmmMappingInfo mapping = {};
    if (!mapped_heap_address(address, &mapping)) { unlock_heap(flags); return; }

    uintptr_t page_base = address & ~(PAGE_SIZE - 1);
    auto* slab = reinterpret_cast<SlabPage*>(page_base);
    if (slab->magic == SLAB_MAGIC) {
        size_t index = 0;
        if (!slab_slot_index(slab, address, index) || !slot_is_allocated(slab, index)) {
            unlock_heap(flags);
            return;
        }
        clear_slot_allocated(slab, index);
        --slab->allocated_count;
        SlubCache& cache = s_caches[slab->cache_index];
        auto* object = reinterpret_cast<FreeObject*>(address);
        object->next = cache.free_list;
        cache.free_list = object;
        if (!slab->allocated_count) reclaim_empty_slab(slab, cache);
        unlock_heap(flags);
        return;
    }

    auto* header_link = reinterpret_cast<LargeAllocHeader**>(address - sizeof(LargeAllocHeader*));
    LargeAllocHeader* header = *header_link;
    uintptr_t header_address = reinterpret_cast<uintptr_t>(header);
    if (header_address < heap_base() || header_address > s_heap_virt_curr - sizeof(LargeAllocHeader)) {
        unlock_heap(flags);
        return;
    }
    VmmMappingInfo header_mapping = {};
    if (!mapped_heap_address(header_address, &header_mapping) || header->magic != LARGE_MAGIC ||
        header->user_pointer != address || header->order > MAX_ORDER) {
        unlock_heap(flags);
        return;
    }
    size_t pages = size_t(1) << header->order;
    uintptr_t base = header->base_virtual;
    uintptr_t physical = header->physical_base;
    uint32_t order = header->order;
    if ((base & (PAGE_SIZE - 1)) || base < heap_base() ||
        pages > (s_heap_virt_curr - base) / PAGE_SIZE) {
        unlock_heap(flags);
        return;
    }
    for (size_t i = 0; i < pages; ++i) {
        VmmMappingInfo page_info = {};
        if (!vmm_query_mapping(base + i * PAGE_SIZE, &page_info) ||
            page_info.physical_page != physical + i * PAGE_SIZE) {
            unlock_heap(flags);
            return;
        }
    }
    header->magic = FREED_LARGE_MAGIC;
    for (size_t i = 0; i < pages; ++i) {
        if (vmm_unmap_page(vmm_get_kernel_pml4(), base + i * PAGE_SIZE) != VmmUnmapResult::Success) {
            unlock_heap(flags);
            return;
        }
    }
    pmm_free(physical, order);
    unlock_heap(flags);
}

void* operator new(size_t size) { return kmalloc(size); }
void* operator new[](size_t size) { return kmalloc(size); }
void operator delete(void* p) noexcept { kfree(p); }
void operator delete[](void* p) noexcept { kfree(p); }
void operator delete(void* p, size_t) noexcept { kfree(p); }
void operator delete[](void* p, size_t) noexcept { kfree(p); }
void* operator new(size_t size, std::align_val_t alignment) {
    return kmalloc_aligned(size, static_cast<size_t>(alignment));
}
void* operator new[](size_t size, std::align_val_t alignment) {
    return kmalloc_aligned(size, static_cast<size_t>(alignment));
}
void operator delete(void* p, std::align_val_t) noexcept { kfree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { kfree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { kfree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { kfree(p); }

bool Heap::start() {
#ifdef DRAKOS_HOST_TEST
    s_heap_virt_curr = g_heap_test_virtual_base;
#else
    s_heap_virt_curr = HEAP_VIRT_BASE;
#endif
    for (size_t i = 0; i < NUM_SLUB_CACHES; ++i) {
        s_caches[i].object_size = SLUB_SIZES[i];
        s_caches[i].free_list = nullptr;
    }
    return true;
}
void Heap::stop() {}
const char* Heap::get_name() const { return "Heap (SLUB Allocator)"; }
static const char* const kHeapDependencies[] = {"g_pmm", "g_vmm"};
REGISTER_MODULE_REQUIRED(g_heap, Heap, 2_mem_c, kHeapDependencies);
