#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "memory/heap.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"

alignas(PAGE_SIZE) static uint8_t s_test_heap[512 * PAGE_SIZE];
uintptr_t g_heap_test_virtual_base = reinterpret_cast<uintptr_t>(s_test_heap);

static bool s_fail_pmm = false;
static uint32_t s_pmm_calls = 0;
static uintptr_t s_next_phys = 0x100000;
static size_t s_live_pages = 0;
struct FakeBlock { uintptr_t base; uint32_t order; bool live; };
static FakeBlock s_blocks[4096];
static size_t s_block_count = 0;

struct FakeMapping {
    uintptr_t virtual_page;
    uintptr_t physical_page;
    VmmMappingInfo::Ownership ownership;
    bool live;
};
static FakeMapping s_mappings[8192];

uintptr_t pmm_alloc(uint32_t order) {
    ++s_pmm_calls;
    if (s_fail_pmm || order > MAX_ORDER || s_block_count == 4096) return 0;
    uintptr_t size = PAGE_SIZE << order;
    s_next_phys = (s_next_phys + size - 1) & ~(size - 1);
    uintptr_t result = s_next_phys;
    s_next_phys += size;
    s_blocks[s_block_count++] = {result, order, true};
    s_live_pages += size / PAGE_SIZE;
    return result;
}

void pmm_free(uintptr_t address, uint32_t order) {
    for (size_t i = 0; i < s_block_count; ++i) {
        if (s_blocks[i].live && s_blocks[i].base == address && s_blocks[i].order == order) {
            s_blocks[i].live = false;
            s_live_pages -= (PAGE_SIZE << order) / PAGE_SIZE;
            return;
        }
    }
}
uint64_t pmm_hhdm_offset() { return 0; }
bool pmm_is_allocated_page(uintptr_t address) {
    for (size_t i = 0; i < s_block_count; ++i)
        if (s_blocks[i].live && s_blocks[i].base == address && s_blocks[i].order == 0) return true;
    return false;
}

static bool add_mapping(uintptr_t virt, uintptr_t phys, VmmMappingInfo::Ownership ownership) {
    for (auto& mapping : s_mappings) {
        if (!mapping.live) {
            mapping = {virt & ~(PAGE_SIZE - 1), phys, ownership, true};
            return true;
        }
    }
    return false;
}
VmmMapResult vmm_map(uintptr_t virt, uintptr_t phys, uint64_t) {
    return add_mapping(virt, phys, VmmMappingInfo::Ownership::Borrowed)
        ? VmmMapResult::Success : VmmMapResult::PageTableOutOfMemory;
}
VmmMapResult vmm_map_page(uintptr_t, uintptr_t virt, uintptr_t phys, uint64_t) {
    return vmm_map(virt, phys, 0);
}
VmmMapResult vmm_map_owned_page(uintptr_t, uintptr_t virt, uintptr_t phys, uint64_t) {
    return add_mapping(virt, phys, VmmMappingInfo::Ownership::Owned)
        ? VmmMapResult::Success : VmmMapResult::PageTableOutOfMemory;
}
uintptr_t vmm_create_address_space() { return 0; }
VmmAddressSpaceResult vmm_switch_address_space(uintptr_t) { return VmmAddressSpaceResult::Success; }
uintptr_t vmm_get_kernel_pml4() { return 1; }
bool vmm_query_mapping(uintptr_t virt, VmmMappingInfo* info) {
    if (!info) return false;
    uintptr_t page = virt & ~(PAGE_SIZE - 1);
    for (auto& mapping : s_mappings) {
        if (mapping.live && mapping.virtual_page == page) {
            info->physical_page = mapping.physical_page;
            info->flags = VMM_PRESENT | VMM_WRITE;
            info->page_size = PAGE_SIZE;
            info->ownership = mapping.ownership;
            return true;
        }
    }
    return false;
}
VmmUnmapResult vmm_unmap(uintptr_t virt) {
    return vmm_unmap_page(1, virt);
}
VmmUnmapResult vmm_unmap_page(uintptr_t, uintptr_t virt) {
    uintptr_t page = virt & ~(PAGE_SIZE - 1);
    for (auto& mapping : s_mappings) {
        if (!mapping.live || mapping.virtual_page != page) continue;
        uintptr_t physical = mapping.physical_page;
        bool owned = mapping.ownership == VmmMappingInfo::Ownership::Owned;
        mapping.live = false;
        if (owned) pmm_free_page(physical);
        return VmmUnmapResult::Success;
    }
    return VmmUnmapResult::NotMapped;
}
uintptr_t vmm_get_phys(uintptr_t) { return 0; }

static bool check(bool condition, const char* message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

int main() {
    Heap heap;
    bool ok = check(heap.start(), "heap initializes");
    ok &= check(kmalloc(0) == nullptr, "zero-size request returns null");

    void* a = kmalloc(8);
    void* c = kmalloc(8);
    void* b = kmalloc(24);
    ok &= check(a && b && c && reinterpret_cast<uintptr_t>(a) % 16 == 0 &&
                reinterpret_cast<uintptr_t>(b) % 16 == 0,
                "small allocations are max-align aligned");
    ok &= check(s_live_pages == 2, "different size classes allocate separate slab pages");
    kfree(a);
    kfree(a); // duplicate free must not insert the same object twice.
    void* reused = kmalloc(8);
    ok &= check(reused == a, "a freed small object is reused without allocating another slab");
    kfree(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(b) + 1));
    kfree(reinterpret_cast<void*>(0x1234));
    kfree(reused);
    kfree(c);
    kfree(b);
    ok &= check(s_live_pages == 0, "empty slabs return their pages to the PMM");
    VmmMappingInfo reclaimed = {};
    ok &= check(!vmm_query_mapping(reinterpret_cast<uintptr_t>(a), &reclaimed),
                "reclaimed slab mapping is removed");

    void* smalls[400];
    for (size_t i = 0; i < 400; ++i) smalls[i] = kmalloc(32);
    ok &= check(smalls[0] && smalls[399], "multiple slabs support repeated small allocation");
    for (size_t i = 0; i < 400; i += 2) kfree(smalls[i]);
    for (size_t i = 1; i < 400; i += 2) kfree(smalls[i]);
    ok &= check(s_live_pages == 0, "fragmented slabs are reclaimed after the final free");

    void* large = kmalloc(2049);
    void* large_multi = kmalloc(3 * PAGE_SIZE);
    ok &= check(large && large_multi, "single-page and multi-page large blocks allocate");
    size_t live_with_large = s_live_pages;
    ok &= check(live_with_large >= 5, "large blocks retain their PMM backing pages");
    kfree(large);
    kfree(large);
    kfree(large_multi);
    ok &= check(s_live_pages == 0, "large deallocation returns full buddy blocks to the PMM");

    void* over_aligned = kmalloc_aligned(80, 256);
    ok &= check(over_aligned && reinterpret_cast<uintptr_t>(over_aligned) % 256 == 0,
                "over-aligned allocations honor their requested alignment");
    kfree(over_aligned);
    ok &= check(s_live_pages == 0, "over-aligned allocation frees its backing block");

    uint32_t calls_before_reject = s_pmm_calls;
    ok &= check(kmalloc(static_cast<size_t>(-1)) == nullptr,
                "overflowing request fails without touching PMM");
    ok &= check(kmalloc((1ULL << MAX_ORDER) * PAGE_SIZE) == nullptr,
                "oversized request exceeds the largest buddy block");
    ok &= check(kmalloc_aligned(64, 3) == nullptr, "non-power-of-two alignment is rejected");
    ok &= check(s_pmm_calls == calls_before_reject, "invalid sizes and alignments do not allocate");

    s_fail_pmm = true;
    ok &= check(kmalloc(128) == nullptr, "small allocation propagates PMM exhaustion");
    ok &= check(kmalloc(4096) == nullptr, "large allocation propagates PMM exhaustion");
    s_fail_pmm = false;
    ok &= check(s_live_pages == 0, "failed allocations preserve PMM accounting");

    kfree(nullptr);
    if (!ok) return 1;
    puts("Heap allocation, deallocation, alignment, reuse, invalid-free, and PMM accounting checks passed.");
    return 0;
}
