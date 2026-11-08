#include "memory/pmm.hpp"
#include "kernel/limine_requests.hpp"

#include <cstdio>
#include <cstdlib>

volatile struct limine_memmap_request g_memmap_request = {};
volatile struct limine_hhdm_request g_hhdm_request = {};

static constexpr uintptr_t PHYS_BASE = 0x100000;
static constexpr size_t ARENA_SIZE = 0x800000;

static bool check(bool condition, const char* message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

static bool in_usable_range(uintptr_t phys, size_t size) {
    const uintptr_t end = phys + size;
    return (phys >= 0x102000 && end <= 0x300000) ||
           (phys >= 0x401000 && end <= 0x900000);
}

int main() {
    void* arena = aligned_alloc(PAGE_SIZE, ARENA_SIZE);
    if (!arena) return 2;

    limine_hhdm_response hhdm = {};
    hhdm.offset = reinterpret_cast<uintptr_t>(arena) - PHYS_BASE;
    g_hhdm_request.response = &hhdm;

    limine_memmap_entry usable_low = {0x100003, 0x1FFFFD, LIMINE_MEMMAP_USABLE};
    limine_memmap_entry reserved = {0x300000, 0x100000, LIMINE_MEMMAP_RESERVED};
    limine_memmap_entry usable_high = {0x400001, 0x500002, LIMINE_MEMMAP_USABLE};
    limine_memmap_entry* entries[] = {&usable_low, &reserved, &usable_high};
    limine_memmap_response memmap = {};
    memmap.entry_count = 3;
    memmap.entries = entries;
    g_memmap_request.response = &memmap;

    PMM pmm;
    bool ok = check(pmm.start(), "PMM initializes from the supplied memory map");
    if (!ok) {
        free(arena);
        return 1;
    }

    uintptr_t one_page = pmm_alloc_page();
    uintptr_t two_pages = pmm_alloc(1);
    ok &= check(one_page != 0 && (one_page & (PAGE_SIZE - 1)) == 0,
                "single-page allocation is nonzero and page aligned");
    ok &= check(two_pages != 0 && (two_pages & ((PAGE_SIZE << 1) - 1)) == 0,
                "order-one allocation is aligned to its two-page block size");
    ok &= check(in_usable_range(one_page, PAGE_SIZE), "single-page allocation avoids reserved memory");
    ok &= check(in_usable_range(two_pages, 2 * PAGE_SIZE), "multi-page allocation avoids reserved memory");
    ok &= check(pmm_alloc(MAX_ORDER + 1) == 0, "orders above the buddy limit fail cleanly");
    ok &= check(pmm_alloc(UINT32_MAX) == 0, "extreme order fails without shifting or indexing out of range");

    // Invalid and mismatched frees must not add memory to the free lists.
    pmm_free(one_page + 1, 0);
    pmm_free(one_page, 1);
    pmm_free(PHYS_BASE + ARENA_SIZE + PAGE_SIZE, 0);
    pmm_free_page(one_page);
    ok &= check(!pmm_is_allocated_page(one_page),
                "a freed page is no longer reported as an allocated head after buddy coalescing");
    pmm_free_page(one_page); // duplicate free must be ignored
    pmm_free(two_pages, 1);

    uintptr_t allocations[2048];
    size_t allocation_count = 0;
    for (;;) {
        uintptr_t page = pmm_alloc_page();
        if (!page) break;
        if (allocation_count == sizeof(allocations) / sizeof(allocations[0])) {
            ok &= check(false, "test allocation buffer can hold the simulated memory");
            break;
        }
        allocations[allocation_count++] = page;
    }

    bool unique = true;
    for (size_t i = 0; i < allocation_count; ++i) {
        for (size_t j = i + 1; j < allocation_count; ++j) {
            if (allocations[i] == allocations[j]) unique = false;
        }
    }
    ok &= check(unique, "exhaustion returns no page more than once");
    ok &= check(allocation_count == 1789, "normalized usable ranges expose the expected number of pages");
    ok &= check(pmm_alloc_page() == 0, "allocation reports exhaustion");
    for (size_t i = 0; i < allocation_count; ++i) {
        ok &= check(in_usable_range(allocations[i], PAGE_SIZE), "exhaustion never allocates a reserved or partial page");
    }

    limine_memmap_entry overflowing = {UINT64_MAX - 3, 8, LIMINE_MEMMAP_USABLE};
    limine_memmap_entry* invalid_entries[] = {&overflowing};
    limine_memmap_response invalid_map = {};
    invalid_map.entry_count = 1;
    invalid_map.entries = invalid_entries;
    g_memmap_request.response = &invalid_map;
    PMM invalid_pmm;
    ok &= check(!invalid_pmm.start(), "overflowing memory-map ranges are rejected");

    free(arena);
    if (!ok) return 1;
    puts("PMM allocation, alignment, reservation, exhaustion, and free checks passed.");
    return 0;
}
