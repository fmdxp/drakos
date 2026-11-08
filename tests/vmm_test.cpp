#include "memory/pmm.hpp"
#include "memory/vmm.hpp"
#include "kernel/limine_requests.hpp"
#include "kernel/idt.hpp"

#include <cstdio>
#include <cstdlib>

volatile struct limine_memmap_request g_memmap_request = {};
volatile struct limine_hhdm_request g_hhdm_request = {};

bool smp_tlb_invalidate_page(uintptr_t) { return true; }
bool smp_tlb_flush_non_global() { return true; }
bool smp_address_space_active(uintptr_t) { return false; }
[[noreturn]] void panic(const char*, InterruptFrame*) { std::abort(); }

struct TestPageTable { uint64_t entries[512]; };
static constexpr uintptr_t PHYS_BASE = 0x100000;
static constexpr size_t ARENA_SIZE = 0x1000000;
static uint64_t HHDM_OFFSET;

static bool check(bool condition, const char* message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

static TestPageTable* table(uintptr_t phys) {
    return reinterpret_cast<TestPageTable*>(phys + HHDM_OFFSET);
}

static uint64_t leaf_entry(uintptr_t root, uintptr_t virt) {
    uint64_t e4 = table(root)->entries[(virt >> 39) & 0x1FF];
    if (!(e4 & VMM_PRESENT)) return 0;
    uint64_t e3 = table(e4 & 0x000FFFFFFFFFF000ULL)->entries[(virt >> 30) & 0x1FF];
    if (!(e3 & VMM_PRESENT)) return 0;
    uint64_t e2 = table(e3 & 0x000FFFFFFFFFF000ULL)->entries[(virt >> 21) & 0x1FF];
    if (!(e2 & VMM_PRESENT)) return 0;
    return table(e2 & 0x000FFFFFFFFFF000ULL)->entries[(virt >> 12) & 0x1FF];
}

static void zero_table(uintptr_t phys) {
    for (size_t i = 0; i < 512; ++i) table(phys)->entries[i] = 0;
}

static uint64_t install_2m_mapping(uintptr_t root, uintptr_t virt, uintptr_t phys_base,
                                   bool writable = true) {
    uintptr_t pdpt_phys = pmm_alloc_page();
    uintptr_t pd_phys = pmm_alloc_page();
    if (!pdpt_phys || !pd_phys) return 0;
    zero_table(pdpt_phys);
    zero_table(pd_phys);
    table(root)->entries[(virt >> 39) & 0x1FF] = pdpt_phys | VMM_PRESENT | VMM_WRITE;
    table(pdpt_phys)->entries[(virt >> 30) & 0x1FF] = pd_phys | VMM_PRESENT | VMM_WRITE;
    uint64_t flags = VMM_PRESENT | (1ULL << 3) | (1ULL << 4) | (1ULL << 7) | (1ULL << 12);
    if (writable) flags |= VMM_WRITE;
    uint64_t original = (phys_base & 0x000FFFFFFFE00000ULL) | flags;
    table(pd_phys)->entries[(virt >> 21) & 0x1FF] = original;
    return original;
}

static uint64_t install_1g_mapping(uintptr_t root, uintptr_t virt, uintptr_t phys_base) {
    uintptr_t pdpt_phys = pmm_alloc_page();
    if (!pdpt_phys) return 0;
    zero_table(pdpt_phys);
    table(root)->entries[(virt >> 39) & 0x1FF] = pdpt_phys | VMM_PRESENT | VMM_WRITE;
    constexpr uint64_t flags = VMM_PRESENT | VMM_WRITE | (1ULL << 3) | (1ULL << 4) |
                               (1ULL << 7) | (1ULL << 12);
    uint64_t original = (phys_base & 0x000FFFFFC0000000ULL) | flags;
    table(pdpt_phys)->entries[(virt >> 30) & 0x1FF] = original;
    return original;
}

static size_t available_pages() {
    static uintptr_t pages[4096];
    size_t count = 0;
    while (count < sizeof(pages) / sizeof(pages[0])) {
        uintptr_t page = pmm_alloc_page();
        if (!page) break;
        pages[count++] = page;
    }
    for (size_t i = count; i > 0; --i) pmm_free_page(pages[i - 1]);
    return count;
}

int main() {
    void* arena = aligned_alloc(PAGE_SIZE, ARENA_SIZE);
    if (!arena) return 2;

    limine_hhdm_response hhdm = {};
    hhdm.offset = reinterpret_cast<uintptr_t>(arena) - PHYS_BASE;
    HHDM_OFFSET = hhdm.offset;
    g_hhdm_request.response = &hhdm;

    limine_memmap_entry usable = {PHYS_BASE, ARENA_SIZE, LIMINE_MEMMAP_USABLE};
    limine_memmap_entry* entries[] = {&usable};
    limine_memmap_response memmap = {};
    memmap.entry_count = 1;
    memmap.entries = entries;
    g_memmap_request.response = &memmap;

    PMM pmm;
    if (!pmm.start()) {
        free(arena);
        return 1;
    }

    uintptr_t limine_root_phys = pmm_alloc_page();
    if (!limine_root_phys) {
        free(arena);
        return 1;
    }
    TestPageTable* limine_root = table(limine_root_phys);
    for (size_t i = 0; i < 512; ++i) limine_root->entries[i] = 0;
    vmm_test_set_cr3(limine_root_phys);

    VMM vmm;
    bool ok = check(vmm.start(), "VMM creates a kernel page table over the fake PMM");
    uintptr_t kernel_root = vmm_get_kernel_pml4();
    uintptr_t phys = pmm_alloc_page();
    constexpr uintptr_t kernel_va = 0xFFFF900000001000ULL;
    constexpr uintptr_t offset = 0x321;

    ok &= check(vmm_map(kernel_va, phys, VMM_PRESENT | VMM_WRITE) == VmmMapResult::Success,
                "valid kernel mapping succeeds");
    ok &= check(vmm_get_phys(kernel_va + offset) == phys + offset,
                "translation includes the byte offset within a page");
    uint64_t kernel_leaf = leaf_entry(kernel_root, kernel_va);
    ok &= check((kernel_leaf & (VMM_PRESENT | VMM_WRITE | VMM_USER)) == (VMM_PRESENT | VMM_WRITE),
                "kernel mapping retains its requested leaf permissions");
    ok &= check(vmm_test_last_invalidation() == kernel_va,
                "active address-space mapping invalidates its TLB entry");

    ok &= check(vmm_map(kernel_va + 1, phys, VMM_KERNEL_RW) == VmmMapResult::InvalidVirtualAddress,
                "unaligned virtual addresses are rejected");
    ok &= check(vmm_map(kernel_va + PAGE_SIZE, phys + 1, VMM_KERNEL_RW) ==
                VmmMapResult::InvalidPhysicalAddress, "unaligned physical addresses are rejected");
    ok &= check(vmm_map(kernel_va + PAGE_SIZE, 0, VMM_KERNEL_RW) ==
                VmmMapResult::InvalidPhysicalAddress, "the reserved physical zero page is rejected");
    ok &= check(vmm_map(0x0000800000000000ULL, phys, VMM_KERNEL_RW) ==
                VmmMapResult::InvalidVirtualAddress, "non-canonical virtual addresses are rejected");
    ok &= check(vmm_map(kernel_va, phys, VMM_KERNEL_RW) == VmmMapResult::MappingExists,
                "an existing leaf mapping is never overwritten");
    ok &= check(vmm_map(kernel_va + PAGE_SIZE, phys, VMM_PRESENT | VMM_NX) ==
                VmmMapResult::Success, "NX kernel mappings are represented in leaf entries");
    ok &= check((leaf_entry(kernel_root, kernel_va + PAGE_SIZE) & VMM_NX) != 0,
                "NX permission is preserved in the leaf entry");
    ok &= check(vmm_map(kernel_va + PAGE_SIZE, phys, VMM_PRESENT | (1ULL << 9)) ==
                VmmMapResult::UnsupportedMapping, "unknown PTE flags are rejected");
    ok &= check(vmm_map(kernel_va + PAGE_SIZE, phys, VMM_PRESENT | VMM_USER) ==
                VmmMapResult::UnsupportedMapping, "user permissions are rejected in the kernel range");
    ok &= check(vmm_map_page(0, kernel_va, phys, VMM_KERNEL_RW) == VmmMapResult::InvalidAddressSpace,
                "a null PML4 is rejected");
    ok &= check(vmm_map_page(0x200000, kernel_va, phys, VMM_KERNEL_RW) ==
                VmmMapResult::InvalidAddressSpace,
                "an aligned physical address that is not an allocated PML4 page is rejected");

    uintptr_t process_root = vmm_create_address_space();
    uintptr_t user_phys = pmm_alloc_page();
    constexpr uintptr_t user_va = 0x00400000;
    ok &= check(vmm_map_page(process_root, user_va, user_phys,
                             VMM_PRESENT | VMM_WRITE | VMM_USER) == VmmMapResult::Success,
                "valid user mapping succeeds");
    vmm_switch_address_space(process_root);
    ok &= check(vmm_get_phys(user_va + offset) == user_phys + offset,
                "current process address-space translation resolves");
    uint64_t user_leaf = leaf_entry(process_root, user_va);
    ok &= check((user_leaf & (VMM_PRESENT | VMM_WRITE | VMM_USER)) ==
                (VMM_PRESENT | VMM_WRITE | VMM_USER), "user mapping preserves user and write permissions");
    uint64_t user_pml4e = table(process_root)->entries[(user_va >> 39) & 0x1FF];
    uint64_t user_pdpte = table(user_pml4e & 0x000FFFFFFFFFF000ULL)->entries[(user_va >> 30) & 0x1FF];
    uint64_t user_pde = table(user_pdpte & 0x000FFFFFFFFFF000ULL)->entries[(user_va >> 21) & 0x1FF];
    ok &= check((user_pml4e & VMM_USER) && (user_pdpte & VMM_USER) && (user_pde & VMM_USER),
                "all user page-table levels permit the requested user leaf");
    ok &= check(!(table(kernel_root)->entries[(kernel_va >> 39) & 0x1FF] & VMM_USER),
                "kernel-only page-table hierarchy stays supervisor-only");
    ok &= check(vmm_get_phys(kernel_va + offset) == phys + offset,
                "process address space sees the shared kernel mapping");

    vmm_unmap(user_va);
    ok &= check(vmm_get_phys(user_va) == 0, "unmap removes the current address-space mapping");
    ok &= check(vmm_test_last_invalidation() == user_va,
                "unmap invalidates the active TLB entry");
    ok &= check(vmm_unmap(user_va) == VmmUnmapResult::NotMapped &&
        vmm_unmap(user_va + 1) == VmmUnmapResult::InvalidVirtualAddress,
        "unmap reports absent mappings and rejects unaligned addresses");
    vmm_switch_address_space(kernel_root);
    ok &= check(vmm_get_phys(user_va) == 0, "user mapping is isolated from the kernel address space");
    ok &= check(pmm_is_allocated_page(user_phys), "borrowed mapping unmap preserves its caller-owned frame");
    pmm_free_page(user_phys);

    // Explicit ownership permits safe frame release and private table reclamation.
    uintptr_t lifecycle_root = vmm_create_address_space();
    uintptr_t owned_a = pmm_alloc_page();
    uintptr_t owned_b = pmm_alloc_page();
    uintptr_t borrowed = pmm_alloc_page();
    constexpr uintptr_t lifecycle_va = 0x2000000;
    ok &= check(lifecycle_root && vmm_map_owned_page(lifecycle_root, lifecycle_va, owned_a,
        VMM_PRESENT | VMM_WRITE | VMM_USER) == VmmMapResult::Success,
        "owned user mapping succeeds");
    ok &= check(vmm_map_owned_page(lifecycle_root, lifecycle_va + 3 * PAGE_SIZE, owned_a,
        VMM_PRESENT | VMM_USER) == VmmMapResult::FrameAlreadyOwned,
        "a frame already owned by another mapping cannot gain a second owned alias");
    ok &= check(vmm_map_owned_page(lifecycle_root, lifecycle_va + PAGE_SIZE, owned_b,
        VMM_PRESENT | VMM_USER) == VmmMapResult::Success,
        "second owned page shares existing intermediate tables");
    ok &= check(vmm_map_page(lifecycle_root, lifecycle_va + 2 * PAGE_SIZE, borrowed,
        VMM_PRESENT | VMM_USER) == VmmMapResult::Success,
        "borrowed user mapping succeeds alongside owned pages");
    size_t before_unmap = available_pages();
    ok &= check(vmm_unmap_page(lifecycle_root, lifecycle_va) == VmmUnmapResult::Success,
        "inactive address-space unmap succeeds");
    ok &= check(!pmm_is_allocated_page(owned_a) && pmm_is_allocated_page(owned_b) &&
        pmm_is_allocated_page(borrowed), "unmap releases only the explicitly owned frame");
    ok &= check(vmm_get_phys(lifecycle_va + PAGE_SIZE) == 0,
        "inactive unmap is visible after a later CR3 switch");
    (void)before_unmap;
    ok &= check(vmm_retain_address_space(lifecycle_root) == VmmAddressSpaceResult::Success &&
        vmm_destroy_address_space(lifecycle_root) == VmmAddressSpaceResult::AddressSpaceInUse,
        "referenced address space cannot be destroyed");
    vmm_release_address_space(lifecycle_root);
    ok &= check(vmm_switch_address_space(lifecycle_root) == VmmAddressSpaceResult::Success &&
        vmm_destroy_address_space(lifecycle_root) == VmmAddressSpaceResult::ActiveAddressSpace,
        "active address space cannot be destroyed");
    vmm_switch_address_space(kernel_root);
    ok &= check(vmm_destroy_address_space(lifecycle_root) == VmmAddressSpaceResult::Success,
        "inactive unreferenced address space is destroyed");
    ok &= check(!pmm_is_allocated_page(owned_b) && pmm_is_allocated_page(borrowed),
        "destruction releases remaining owned frame and preserves borrowed frame");
    ok &= check(vmm_destroy_address_space(lifecycle_root) == VmmAddressSpaceResult::InvalidAddressSpace,
        "repeated destruction is rejected");
    pmm_free_page(borrowed);

    uintptr_t reclaim_root = vmm_create_address_space();
    uintptr_t reclaim_frame_a = pmm_alloc_page();
    uintptr_t reclaim_frame_b = pmm_alloc_page();
    constexpr uintptr_t reclaim_va = 0x04000000;
    ok &= check(vmm_map_page(reclaim_root, reclaim_va, reclaim_frame_a,
        VMM_PRESENT | VMM_USER) == VmmMapResult::Success &&
        vmm_map_page(reclaim_root, reclaim_va + PAGE_SIZE, reclaim_frame_b,
        VMM_PRESENT | VMM_USER) == VmmMapResult::Success,
        "sibling borrowed mappings share private page tables");
    size_t reclaim_free_before = available_pages();
    ok &= check(vmm_unmap_page(reclaim_root, reclaim_va) == VmmUnmapResult::Success &&
        (leaf_entry(reclaim_root, reclaim_va + PAGE_SIZE) & 0x000FFFFFFFFFF000ULL) == reclaim_frame_b,
        "unmapping one sibling preserves its neighbor and shared tables");
    ok &= check(vmm_unmap_page(reclaim_root, reclaim_va + PAGE_SIZE) == VmmUnmapResult::Success &&
        leaf_entry(reclaim_root, reclaim_va + PAGE_SIZE) == 0 &&
        pmm_is_allocated_page(reclaim_frame_a) && pmm_is_allocated_page(reclaim_frame_b),
        "last unmap reclaims empty tables without freeing borrowed frames");
    ok &= check(available_pages() >= reclaim_free_before + 3,
        "empty PT, PD, and PDPT pages return to the PMM");
    vmm_destroy_address_space(reclaim_root);
    pmm_free_page(reclaim_frame_a);
    pmm_free_page(reclaim_frame_b);

    uintptr_t empty_root = vmm_create_address_space();
    ok &= check(vmm_destroy_address_space(kernel_root) == VmmAddressSpaceResult::KernelAddressSpace,
        "kernel address-space destruction is rejected");
    ok &= check(empty_root && vmm_destroy_address_space(empty_root) == VmmAddressSpaceResult::Success,
        "empty address space is reclaimed");

    // Fail each of the three possible page-table page allocations. The test
    // hook fails the next VMM table allocation while PMM remains real.
    constexpr uintptr_t failure_vas[] = {
        0xFFFFA00000000000ULL,
        0xFFFFA08000000000ULL,
        0xFFFFA10000000000ULL,
    };
    for (int failed_level = 0; failed_level < 3; ++failed_level) {
        uintptr_t caller_frame = pmm_alloc_page();
        if (!caller_frame) { ok = false; break; }
        size_t free_before = available_pages();
        vmm_test_fail_page_table_allocation_after(failed_level);
        VmmMapResult result = vmm_map_page(kernel_root, failure_vas[failed_level], caller_frame,
                                           VMM_KERNEL_RW);
        vmm_test_fail_page_table_allocation_after(-1);
        ok &= check(result == VmmMapResult::PageTableOutOfMemory,
                    "intermediate page-table allocation failure is reported");
        ok &= check(table(kernel_root)->entries[(failure_vas[failed_level] >> 39) & 0x1FF] == 0,
                    "failed mapping leaves its previously empty root slot unchanged");
        ok &= check(available_pages() == free_before,
                    "all newly allocated intermediate pages are returned on rollback");
        pmm_free_page(caller_frame);
    }

    // Existing hierarchy and mapping must survive a failure below a shared
    // PML4 entry. This also checks that rollback never frees old table pages.
    uintptr_t preserved_root_entry = table(kernel_root)->entries[(kernel_va >> 39) & 0x1FF];
    uintptr_t preserved_phys = pmm_alloc_page();
    constexpr uintptr_t preserved_neighbor_va = 0xFFFF904000000000ULL;
    vmm_test_fail_page_table_allocation_after(0);
    VmmMapResult preserved_failure = vmm_map_page(kernel_root, preserved_neighbor_va,
                                                   preserved_phys, VMM_KERNEL_RW);
    vmm_test_fail_page_table_allocation_after(-1);
    ok &= check(preserved_failure == VmmMapResult::PageTableOutOfMemory,
                "allocation failure below an existing table is reported");
    ok &= check(table(kernel_root)->entries[(kernel_va >> 39) & 0x1FF] == preserved_root_entry,
                "rollback preserves page tables that predate the mapping");
    ok &= check(vmm_get_phys(kernel_va) == phys,
                "an existing mapping remains usable after a neighboring failure");
    pmm_free_page(preserved_phys);

    // A 2 MiB HHDM mapping is split transactionally and keeps its effective
    // cache and permission attributes for every untouched 4 KiB page.
    constexpr uintptr_t huge2m_va = 0xFFFFA80000000000ULL;
    constexpr uintptr_t huge2m_base = 0x00400000;
    constexpr uintptr_t huge2m_target_va = huge2m_va + 0x3000;
    constexpr uintptr_t huge2m_target_phys = huge2m_base + 0x3000;
    uint64_t original_2m = install_2m_mapping(kernel_root, huge2m_va, huge2m_base);
    VmmMappingInfo original_2m_info = {};
    ok &= check(original_2m != 0 && vmm_get_phys(huge2m_target_va) == huge2m_target_phys,
                "translation resolves an existing 2 MiB mapping");
    ok &= check(vmm_query_mapping(huge2m_target_va, &original_2m_info) &&
                original_2m_info.page_size == (1ULL << 21) &&
                original_2m_info.physical_page == huge2m_target_phys &&
                (original_2m_info.flags & (1ULL << 12)),
                "mapping query reports the 2 MiB leaf, physical page, and large PAT bit");
    ok &= check(vmm_map_page(kernel_root, huge2m_target_va, huge2m_target_phys,
                             VMM_KERNEL_RW) == VmmMapResult::MappingExists,
                "ordinary mapping refuses to overwrite an existing large-page mapping");
    size_t free_before_2m_failure = available_pages();
    vmm_test_fail_page_table_allocation_after(0);
    ok &= check(vmm_map_mmio_page(huge2m_target_va, huge2m_target_phys) ==
                VmmMapResult::PageTableOutOfMemory, "2 MiB split reports allocation failure");
    vmm_test_fail_page_table_allocation_after(-1);
    uint64_t* original_2m_entry = &table(table(table(kernel_root)->entries[(huge2m_va >> 39) & 0x1FF]
        & 0x000FFFFFFFFFF000ULL)->entries[(huge2m_va >> 30) & 0x1FF]
        & 0x000FFFFFFFFFF000ULL)->entries[(huge2m_va >> 21) & 0x1FF];
    ok &= check(*original_2m_entry == original_2m &&
                vmm_get_phys(huge2m_target_va) == huge2m_target_phys,
                "failed 2 MiB split preserves its original entry and translation");
    ok &= check(available_pages() == free_before_2m_failure,
                "failed 2 MiB split returns its temporary page table");
    ok &= check(vmm_map_mmio_page(huge2m_target_va, huge2m_target_phys) == VmmMapResult::Success,
                "2 MiB mapping splits and applies MMIO attributes");
    ok &= check(vmm_get_phys(huge2m_target_va) == huge2m_target_phys,
                "split 2 MiB mapping translates the MMIO page");
    VmmMappingInfo split_2m_info = {};
    ok &= check(vmm_query_mapping(huge2m_target_va, &split_2m_info) &&
                split_2m_info.page_size == PAGE_SIZE &&
                split_2m_info.physical_page == huge2m_target_phys,
                "mapping query reports the resulting 4 KiB MMIO leaf");
    uint64_t target_2m_leaf = leaf_entry(kernel_root, huge2m_target_va);
    uint64_t neighbor_2m_leaf = leaf_entry(kernel_root, huge2m_va + 0x4000);
    ok &= check((target_2m_leaf & (VMM_PRESENT | VMM_WRITE | VMM_CACHE_DISABLE)) == VMM_MMIO &&
                !(target_2m_leaf & (1ULL << 7)),
                "target 2 MiB child receives requested MMIO memory type");
    ok &= check((neighbor_2m_leaf & (VMM_PRESENT | VMM_WRITE | VMM_CACHE_DISABLE | (1ULL << 7))) ==
                (VMM_PRESENT | VMM_WRITE | VMM_CACHE_DISABLE | (1ULL << 7)),
                "neighboring 2 MiB children preserve write, cache, and PAT attributes");
    size_t free_after_2m_split = available_pages();
    ok &= check(vmm_map_mmio_page(huge2m_target_va, huge2m_target_phys) == VmmMapResult::Success &&
                available_pages() == free_after_2m_split,
                "an already suitable HHDM MMIO leaf is reused without allocating or replacing it");
    ok &= check(vmm_map_mmio_page(huge2m_target_va, huge2m_target_phys + PAGE_SIZE) ==
                VmmMapResult::MappingExists && vmm_get_phys(huge2m_target_va) == huge2m_target_phys,
                "MMIO helper refuses to replace an existing mapping to another physical page");
    uintptr_t mmio_test_frame = pmm_alloc_page();
    constexpr uintptr_t mmio_test_va = 0xFFFFAC0000000000ULL;
    ok &= check(vmm_map_mmio_page(mmio_test_va, mmio_test_frame) == VmmMapResult::Success,
        "a new MMIO mapping is explicitly tagged");
    VmmMappingInfo mmio_test_info = {};
    ok &= check(vmm_query_mapping(mmio_test_va, &mmio_test_info) &&
        mmio_test_info.ownership == VmmMappingInfo::Ownership::Mmio,
        "mapping query reports MMIO ownership");
    ok &= check(vmm_unmap_page(kernel_root, mmio_test_va) == VmmUnmapResult::Success &&
        pmm_is_allocated_page(mmio_test_frame),
        "unmapping an MMIO-tagged frame never returns it to the PMM");
    pmm_free_page(mmio_test_frame);
    ok &= check(vmm_unmap_page(kernel_root, huge2m_va + 0x4000) == VmmUnmapResult::Success &&
                vmm_get_phys(huge2m_va + 0x4000) == 0 &&
                vmm_get_phys(huge2m_va + 0x5000) == huge2m_base + 0x5000,
                "unmapping a split large-page child preserves neighboring translations");
    ok &= check(vmm_unmap_page(kernel_root, huge2m_va + 0x4000) == VmmUnmapResult::NotMapped,
                "unmapping an absent page reports NotMapped");

    constexpr uintptr_t readonly2m_va = 0xFFFFAA0000000000ULL;
    constexpr uintptr_t readonly2m_base = 0x00600000;
    constexpr uintptr_t readonly2m_target_va = readonly2m_va + 0x5000;
    constexpr uintptr_t readonly2m_target_phys = readonly2m_base + 0x5000;
    ok &= check(install_2m_mapping(kernel_root, readonly2m_va, readonly2m_base, false) != 0 &&
                vmm_map_mmio_page(readonly2m_target_va, readonly2m_target_phys) == VmmMapResult::Success,
                "read-only large mappings can be split for an explicit writable MMIO page");
    uint64_t readonly_neighbor = leaf_entry(kernel_root, readonly2m_va + 0x6000);
    uint64_t writable_mmio_target = leaf_entry(kernel_root, readonly2m_target_va);
    ok &= check(!(readonly_neighbor & VMM_WRITE) && (writable_mmio_target & VMM_WRITE),
                "splitting preserves neighboring read-only permissions while setting MMIO write access");

    // A 1 GiB entry needs a PD and a PT. Failure at either allocation must
    // leave the original PDPTE intact and restore the PMM free-page count.
    constexpr uintptr_t huge1g_va = 0xFFFFB00000000000ULL;
    constexpr uintptr_t huge1g_target_va = huge1g_va + 0x00803000;
    constexpr uintptr_t huge1g_target_phys = 0x00803000;
    uint64_t original_1g = install_1g_mapping(kernel_root, huge1g_va, 0);
    VmmMappingInfo original_1g_info = {};
    ok &= check(original_1g != 0 && vmm_get_phys(huge1g_target_va) == huge1g_target_phys,
                "translation resolves an existing 1 GiB mapping");
    ok &= check(vmm_query_mapping(huge1g_target_va, &original_1g_info) &&
                original_1g_info.page_size == (1ULL << 30) &&
                original_1g_info.physical_page == huge1g_target_phys &&
                (original_1g_info.flags & (1ULL << 12)),
                "mapping query reports the 1 GiB leaf, physical page, and large PAT bit");
    uint64_t* original_1g_entry = &table(table(kernel_root)->entries[(huge1g_va >> 39) & 0x1FF]
        & 0x000FFFFFFFFFF000ULL)->entries[(huge1g_va >> 30) & 0x1FF];
    for (int fail_after = 0; fail_after < 2; ++fail_after) {
        size_t free_before = available_pages();
        vmm_test_fail_page_table_allocation_after(fail_after);
        VmmMapResult result = vmm_map_mmio_page(huge1g_target_va, huge1g_target_phys);
        vmm_test_fail_page_table_allocation_after(-1);
        ok &= check(result == VmmMapResult::PageTableOutOfMemory,
                    "each 1 GiB split allocation failure is reported");
        ok &= check(*original_1g_entry == original_1g &&
                    vmm_get_phys(huge1g_target_va) == huge1g_target_phys,
                    "failed 1 GiB split preserves original entry and translation");
        ok &= check(available_pages() == free_before,
                    "failed 1 GiB split returns every temporary table page");
    }
    ok &= check(vmm_map_mmio_page(huge1g_target_va, huge1g_target_phys) == VmmMapResult::Success,
                "1 GiB mapping splits through PD and PT levels");
    ok &= check(vmm_get_phys(huge1g_target_va) == huge1g_target_phys,
                "split 1 GiB mapping translates the MMIO page");
    uint64_t target_1g_leaf = leaf_entry(kernel_root, huge1g_target_va);
    uint64_t neighbor_1g_leaf = leaf_entry(kernel_root, huge1g_va + 0x00804000);
    ok &= check((target_1g_leaf & (VMM_PRESENT | VMM_WRITE | VMM_CACHE_DISABLE)) == VMM_MMIO &&
                !(target_1g_leaf & (1ULL << 7)),
                "target 1 GiB child receives requested MMIO memory type");
    ok &= check((neighbor_1g_leaf & (VMM_PRESENT | VMM_WRITE | VMM_CACHE_DISABLE | (1ULL << 7))) ==
                (VMM_PRESENT | VMM_WRITE | VMM_CACHE_DISABLE | (1ULL << 7)),
                "neighboring 1 GiB children preserve write, cache, and PAT attributes");
    ok &= check(vmm_unmap_page(kernel_root, huge1g_target_va) == VmmUnmapResult::Success &&
                vmm_get_phys(huge1g_target_va) == 0 &&
                vmm_get_phys(huge1g_target_va + PAGE_SIZE) == huge1g_target_phys + PAGE_SIZE,
                "unmap after a 1 GiB split removes only the selected 4 KiB child");

    // The first registry chunk fills, the next chunk allocation is injected to
    // fail, and roots already stored in the old chunk remain usable.
    uintptr_t stress_roots[320] = {};
    size_t stress_count = 0;
    vmm_test_fail_registry_allocation_after(0);
    while (stress_count < 320) {
        uintptr_t root = vmm_create_address_space();
        if (!root) break;
        stress_roots[stress_count++] = root;
    }
    vmm_test_fail_registry_allocation_after(-1);
    ok &= check(stress_count > 128 && stress_count < 320,
        "registry grows past the former 128-root limit then reaches injected chunk failure");
    ok &= check(vmm_retain_address_space(stress_roots[0]) == VmmAddressSpaceResult::Success &&
        vmm_release_address_space(stress_roots[0]) == VmmAddressSpaceResult::Success &&
        vmm_switch_address_space(stress_roots[0]) == VmmAddressSpaceResult::Success,
        "existing roots and references remain valid after failed registry growth");
    vmm_switch_address_space(kernel_root);

    uintptr_t reused_root = vmm_create_address_space();
    ok &= check(vmm_destroy_address_space(stress_roots[0]) == VmmAddressSpaceResult::Success &&
        reused_root != 0,
        "destroying a root frees registry capacity that can be reused");
    stress_roots[0] = reused_root;
    constexpr uintptr_t stress_va = 0x08000000;
    uintptr_t stress_frame_a = pmm_alloc_page();
    uintptr_t stress_frame_b = pmm_alloc_page();
    ok &= check(vmm_map_owned_page(stress_roots[1], stress_va, stress_frame_a,
        VMM_PRESENT | VMM_USER) == VmmMapResult::Success &&
        vmm_map_owned_page(stress_roots[128], stress_va, stress_frame_b,
        VMM_PRESENT | VMM_USER) == VmmMapResult::Success,
        "independent owned mappings succeed on roots beyond index 128");
    ok &= check(vmm_retain_address_space(stress_roots[129]) == VmmAddressSpaceResult::Success &&
        vmm_release_address_space(stress_roots[129]) == VmmAddressSpaceResult::Success,
        "reference operations remain correct on roots after chunk growth");

    uintptr_t extra_roots[64] = {};
    size_t extra_count = 0;
    for (; extra_count < 64; ++extra_count) {
        extra_roots[extra_count] = vmm_create_address_space();
        if (!extra_roots[extra_count]) break;
    }
    ok &= check(extra_count > 0, "registry allocates an additional PMM-backed chunk");
    for (size_t i = extra_count; i > 0; --i)
        ok &= check(vmm_destroy_address_space(extra_roots[i - 1]) == VmmAddressSpaceResult::Success,
                    "registry destroys roots from the expanded chunk");
    for (size_t i = 0; i < stress_count; ++i) {
        if (i == 1 || i == 128) continue;
        ok &= check(vmm_destroy_address_space(stress_roots[i]) == VmmAddressSpaceResult::Success,
                    "registry preserves unrelated roots while deleting in varied order");
    }
    ok &= check(vmm_destroy_address_space(stress_roots[1]) == VmmAddressSpaceResult::Success &&
        vmm_destroy_address_space(stress_roots[128]) == VmmAddressSpaceResult::Success &&
        !pmm_is_allocated_page(stress_frame_a) && !pmm_is_allocated_page(stress_frame_b),
        "owned frames in high-index roots are reclaimed after registry shrinking");
    ok &= check(vmm_destroy_address_space(process_root) == VmmAddressSpaceResult::Success,
        "the pre-stress test root is released");
    vmm_test_fail_registry_allocation_after(0);
    size_t free_before_failed_create = available_pages();
    ok &= check(vmm_create_address_space() == 0 && available_pages() == free_before_failed_create,
        "empty-registry allocation failure leaks neither a root nor a chunk");
    vmm_test_fail_registry_allocation_after(-1);
    uintptr_t after_failure_root = vmm_create_address_space();
    ok &= check(after_failure_root &&
        vmm_destroy_address_space(after_failure_root) == VmmAddressSpaceResult::Success,
        "registry can grow again after an allocation failure");

    free(arena);
    if (!ok) return 1;
    puts("VMM lifecycle, dynamic registry, ownership, unmap, and TLB checks passed.");
    return 0;
}
