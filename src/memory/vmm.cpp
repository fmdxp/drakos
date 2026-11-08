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


#include "vmm.hpp"
#include "pmm.hpp"
#include "limine_requests.hpp"
#include "kernel/smp.hpp"
#include "cpu.hpp"
#include "kernel/panic.hpp"

// -------------------------------------------------------------------
// X86_64 PAGING STRUCTURES
// -------------------------------------------------------------------

// Address translation masks
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_FLAGS_MASK 0xFFF0000000000FFFULL
#define PDE_2M_ADDR_MASK   0x000FFFFFFFE00000ULL
#define PDPTE_1G_ADDR_MASK 0x000FFFFFC0000000ULL
#define PAGE_SIZE_2M (1ULL << 21)
#define PAGE_SIZE_1G (1ULL << 30)
#define PAGE_SIZE_2M_MASK (PAGE_SIZE_2M - 1)
#define PAGE_SIZE_1G_MASK (PAGE_SIZE_1G - 1)
#define PTE_PAT_4K (1ULL << 7)
#define PDE_PAT_LARGE (1ULL << 12)
#define PTE_PS (1ULL << 7)

// Page table indices from virtual address
#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

// A single page table has 512 entries
struct PageTable {
    uint64_t entries[512];
};

// -------------------------------------------------------------------
// INTERNAL STATE
// -------------------------------------------------------------------

// The top-level page table (PML4) physical address
static uintptr_t s_kernel_pml4 = 0;
static volatile bool s_vmm_lock = false;
static constexpr uint64_t PTE_OWNER_BORROWED = 1ULL << 9;
static constexpr uint64_t PTE_OWNER_OWNED = 1ULL << 10;
static constexpr uint64_t PTE_OWNER_MMIO = 1ULL << 11;
struct AddressSpaceRecord { uintptr_t root; uint32_t references; };
struct AddressSpaceChunk {
    uintptr_t physical;
    AddressSpaceChunk* next;
    uint32_t count;
    uint32_t reserved;
};
static constexpr size_t ADDRESS_SPACE_CHUNK_CAPACITY =
    (PAGE_SIZE - sizeof(AddressSpaceChunk)) / sizeof(AddressSpaceRecord);
static_assert(sizeof(AddressSpaceChunk) + ADDRESS_SPACE_CHUNK_CAPACITY * sizeof(AddressSpaceRecord) <= PAGE_SIZE);
static AddressSpaceChunk* s_address_space_chunks = nullptr;
#ifdef DRAKOS_HOST_TEST
static int s_test_fail_registry_after = -1;
#endif

#ifdef DRAKOS_HOST_TEST
static uintptr_t s_test_cr3 = 0;
static uintptr_t s_test_last_invalidation = 0;
static int s_test_fail_after = -1;
#endif

// -------------------------------------------------------------------
// INTERNAL HELPERS
// -------------------------------------------------------------------

// Read CR3 (current PML4 physical address)
static inline uintptr_t read_cr3() {
#ifdef DRAKOS_HOST_TEST
    return s_test_cr3;
#else
    uintptr_t val;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
#endif
}

// Write CR3 (load a new PML4)
static inline void write_cr3(uintptr_t val) {
#ifdef DRAKOS_HOST_TEST
    s_test_cr3 = val;
#else
    asm volatile("mov %0, %%cr3" : : "r"(val) : "memory");
#endif
}

// Invalidate a single TLB entry
static inline void invlpg(uintptr_t virt) {
#ifdef DRAKOS_HOST_TEST
    s_test_last_invalidation = virt;
#else
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
#endif
}

static inline uint64_t lock_vmm() {
#ifdef DRAKOS_HOST_TEST
    return 0;
#else
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&s_vmm_lock, __ATOMIC_ACQUIRE)) {
        asm volatile("pause");
    }
    return flags;
#endif
}

static inline void unlock_vmm(uint64_t flags) {
#ifndef DRAKOS_HOST_TEST
    __atomic_clear(&s_vmm_lock, __ATOMIC_RELEASE);
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

static uintptr_t allocate_table_page() {
#ifdef DRAKOS_HOST_TEST
    if (s_test_fail_after == 0) {
        s_test_fail_after = -1;
        return 0;
    }
    if (s_test_fail_after > 0) --s_test_fail_after;
#endif
    return pmm_alloc_page();
}

#ifdef DRAKOS_HOST_TEST
void vmm_test_set_cr3(uintptr_t value) { s_test_cr3 = value; }
uintptr_t vmm_test_last_invalidation() { return s_test_last_invalidation; }
void vmm_test_fail_page_table_allocation_after(int successful_allocations) {
    s_test_fail_after = successful_allocations;
}
void vmm_test_fail_registry_allocation_after(int successful_allocations) {
    s_test_fail_registry_after = successful_allocations;
}
#endif

// -------------------------------------------------------------------
// PUBLIC API IMPLEMENTATION
// -------------------------------------------------------------------

static bool is_canonical(uintptr_t address) {
    const uint64_t upper = static_cast<uint64_t>(address) >> 48;
    const bool sign = (address & (1ULL << 47)) != 0;
    return sign ? upper == 0xFFFF : upper == 0;
}

static bool is_user_address(uintptr_t address) {
    return (address & (1ULL << 47)) == 0;
}

static uint64_t large_leaf_to_4k_flags(uint64_t large_entry) {
    uint64_t flags = large_entry & PTE_FLAGS_MASK;
    flags &= ~PTE_PS;
    if (large_entry & PDE_PAT_LARGE) flags |= PTE_PAT_4K;
    return flags;
}

static uint64_t table_pointer_flags(uint64_t previous_leaf_flags) {
    constexpr uint64_t software_bits = (7ULL << 9) | (0x7FULL << 52);
    uint64_t flags = VMM_PRESENT | VMM_WRITE | (previous_leaf_flags & VMM_USER);
    flags |= previous_leaf_flags & ((1ULL << 5) | VMM_NX | software_bits);
    return flags;
}

static void fill_pt_from_2m(PageTable* pt, uint64_t huge_entry) {
    uintptr_t base = huge_entry & PDE_2M_ADDR_MASK;
    uint64_t flags = large_leaf_to_4k_flags(huge_entry);
    for (size_t i = 0; i < 512; ++i) {
        pt->entries[i] = (base + i * PAGE_SIZE) | flags;
    }
}

static void fill_pd_from_1g(PageTable* pd, uint64_t huge_entry) {
    uintptr_t base = huge_entry & PDPTE_1G_ADDR_MASK;
    uint64_t flags = huge_entry & PTE_FLAGS_MASK;
    flags |= huge_entry & PDE_PAT_LARGE;
    for (size_t i = 0; i < 512; ++i) {
        pd->entries[i] = (base + i * PAGE_SIZE_2M) | flags;
    }
}

static void invalidate_page_all_cpus(uintptr_t virt) {
    invlpg(virt);
    if (!smp_tlb_invalidate_page(virt)) panic("TLB shootdown timed out");
}

static void flush_address_spaces_all_cpus() {
    if (!smp_tlb_flush_non_global()) panic("TLB full flush timed out");
}

static uint64_t mmio_leaf_flags(uint64_t old_flags) {
    constexpr uint64_t replaced = VMM_WRITE | VMM_USER | VMM_CACHE_DISABLE | PTE_PAT_4K |
                                  PTE_OWNER_BORROWED | PTE_OWNER_OWNED | PTE_OWNER_MMIO;
    return (old_flags & PTE_FLAGS_MASK & ~replaced) | VMM_MMIO | PTE_OWNER_MMIO;
}

static AddressSpaceRecord* chunk_records(AddressSpaceChunk* chunk) {
    return reinterpret_cast<AddressSpaceRecord*>(reinterpret_cast<uintptr_t>(chunk) + sizeof(AddressSpaceChunk));
}

static AddressSpaceChunk* allocate_address_space_chunk() {
#ifdef DRAKOS_HOST_TEST
    if (s_test_fail_registry_after == 0) {
        s_test_fail_registry_after = -1;
        return nullptr;
    }
    if (s_test_fail_registry_after > 0) --s_test_fail_registry_after;
#endif
    uintptr_t physical = pmm_alloc_page();
    if (!physical) return nullptr;
    AddressSpaceChunk* chunk = reinterpret_cast<AddressSpaceChunk*>(physical + pmm_hhdm_offset());
    chunk->physical = physical;
    chunk->next = nullptr;
    chunk->count = 0;
    chunk->reserved = 0;
    return chunk;
}

static AddressSpaceRecord* find_address_space(uintptr_t root) {
    for (AddressSpaceChunk* chunk = s_address_space_chunks; chunk; chunk = chunk->next) {
        AddressSpaceRecord* records = chunk_records(chunk);
        for (size_t i = 0; i < chunk->count; ++i)
            if (records[i].root == root) return &records[i];
    }
    return nullptr;
}

static bool register_address_space(uintptr_t root) {
    if (!root || find_address_space(root)) return false;
    AddressSpaceChunk* available = nullptr;
    AddressSpaceChunk* tail = nullptr;
    for (AddressSpaceChunk* chunk = s_address_space_chunks; chunk; chunk = chunk->next) {
        if (chunk->count < ADDRESS_SPACE_CHUNK_CAPACITY) { available = chunk; break; }
        tail = chunk;
    }
    if (!available) {
        available = allocate_address_space_chunk();
        if (!available) return false;
        if (tail) tail->next = available;
        else s_address_space_chunks = available;
    }
    chunk_records(available)[available->count++] = {root, 0};
    return true;
}

static bool unregister_address_space(uintptr_t root) {
    AddressSpaceChunk* previous = nullptr;
    for (AddressSpaceChunk* chunk = s_address_space_chunks; chunk; previous = chunk, chunk = chunk->next) {
        AddressSpaceRecord* records = chunk_records(chunk);
        for (size_t i = 0; i < chunk->count; ++i) {
            if (records[i].root != root) continue;
            records[i] = records[chunk->count - 1];
            --chunk->count;
            if (!chunk->count) {
                if (previous) previous->next = chunk->next;
                else s_address_space_chunks = chunk->next;
                pmm_free_page(chunk->physical);
            }
            return true;
        }
    }
    return false;
}

static bool valid_root(uintptr_t root) {
    return root == s_kernel_pml4 || find_address_space(root) != nullptr;
}

static bool root_owns_frame(uintptr_t root, uintptr_t phys) {
    PageTable* pml4 = reinterpret_cast<PageTable*>(root + pmm_hhdm_offset());
    for (size_t i4 = 0; i4 < 512; ++i4) {
        uint64_t e4 = pml4->entries[i4];
        if (!(e4 & VMM_PRESENT) || (e4 & PTE_PS)) continue;
        PageTable* pdpt = reinterpret_cast<PageTable*>((e4 & PTE_ADDR_MASK) + pmm_hhdm_offset());
        for (size_t i3 = 0; i3 < 512; ++i3) {
            uint64_t e3 = pdpt->entries[i3];
            if (!(e3 & VMM_PRESENT) || (e3 & PTE_PS)) continue;
            PageTable* pd = reinterpret_cast<PageTable*>((e3 & PTE_ADDR_MASK) + pmm_hhdm_offset());
            for (size_t i2 = 0; i2 < 512; ++i2) {
                uint64_t e2 = pd->entries[i2];
                if (!(e2 & VMM_PRESENT) || (e2 & PTE_PS)) continue;
                PageTable* pt = reinterpret_cast<PageTable*>((e2 & PTE_ADDR_MASK) + pmm_hhdm_offset());
                for (size_t i1 = 0; i1 < 512; ++i1) {
                    uint64_t e1 = pt->entries[i1];
                    if ((e1 & (VMM_PRESENT | PTE_OWNER_OWNED)) ==
                            (VMM_PRESENT | PTE_OWNER_OWNED) &&
                        (e1 & PTE_ADDR_MASK) == phys) return true;
                }
            }
        }
    }
    return false;
}

static bool frame_is_owned(uintptr_t phys) {
    if (s_kernel_pml4 && root_owns_frame(s_kernel_pml4, phys)) return true;
    for (AddressSpaceChunk* chunk = s_address_space_chunks; chunk; chunk = chunk->next) {
        AddressSpaceRecord* records = chunk_records(chunk);
        for (size_t i = 0; i < chunk->count; ++i)
            if (root_owns_frame(records[i].root, phys)) return true;
    }
    return false;
}

static VmmMapResult map_page_with_owner(uintptr_t pml4_phys, uintptr_t virt, uintptr_t phys, uint64_t flags, uint64_t owner) {
    if (!pml4_phys || (pml4_phys & (PAGE_SIZE - 1)) || (pml4_phys & ~PTE_ADDR_MASK)) {
        return VmmMapResult::InvalidAddressSpace;
    }
    if (!is_canonical(virt)) return VmmMapResult::InvalidVirtualAddress;
    if (virt & (PAGE_SIZE - 1)) return VmmMapResult::InvalidVirtualAddress;
    if (!phys || (phys & (PAGE_SIZE - 1)) || (phys & ~PTE_ADDR_MASK)) {
        return VmmMapResult::InvalidPhysicalAddress;
    }

    constexpr uint64_t supported_flags = VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_CACHE_DISABLE | VMM_NX;
    if ((flags & ~supported_flags) || !(flags & VMM_PRESENT)) {
        return VmmMapResult::UnsupportedMapping;
    }
#ifndef DRAKOS_HOST_TEST
    if ((flags & VMM_NX) && !nx_enabled()) return VmmMapResult::UnsupportedMapping;
#endif
    uint64_t lock_flags = lock_vmm();
    if (!valid_root(pml4_phys)) {
        unlock_vmm(lock_flags);
        return VmmMapResult::InvalidAddressSpace;
    }
    if (owner == PTE_OWNER_OWNED && frame_is_owned(phys)) {
        unlock_vmm(lock_flags);
        return VmmMapResult::FrameAlreadyOwned;
    }
    const bool user_address = is_user_address(virt);
    if (((flags & VMM_USER) != 0) != user_address ||
        (user_address && pml4_phys == s_kernel_pml4) ||
        (!user_address && pml4_phys != s_kernel_pml4)) {
        unlock_vmm(lock_flags);
        return VmmMapResult::UnsupportedMapping;
    }
    PageTable* tables[4];
    uint16_t indices[3] = {
        static_cast<uint16_t>(PML4_INDEX(virt)),
        static_cast<uint16_t>(PDPT_INDEX(virt)),
        static_cast<uint16_t>(PD_INDEX(virt)),
    };
    uintptr_t newly_allocated[3] = {};
    tables[0] = reinterpret_cast<PageTable*>(pml4_phys + pmm_hhdm_offset());

    for (size_t level = 0; level < 3; ++level) {
        uint64_t entry = tables[level]->entries[indices[level]];
        if (entry & VMM_PRESENT) {
            // PS entries at PDPT or PD level are existing 1 GiB / 2 MiB mappings.
            if (level != 0 && (entry & (1ULL << 7))) {
                unlock_vmm(lock_flags);
                return VmmMapResult::MappingExists;
            }
            uintptr_t next_phys = entry & PTE_ADDR_MASK;
            if (!next_phys) {
                unlock_vmm(lock_flags);
                return VmmMapResult::UnsupportedMapping;
            }
            tables[level + 1] = reinterpret_cast<PageTable*>(next_phys + pmm_hhdm_offset());
            continue;
        }
        if (entry != 0) {
            unlock_vmm(lock_flags);
            return VmmMapResult::UnsupportedMapping;
        }

        uintptr_t table_phys = allocate_table_page();
        if (!table_phys) {
            for (size_t rollback = level; rollback > 0; --rollback) {
                pmm_free(newly_allocated[rollback - 1], 0);
            }
            unlock_vmm(lock_flags);
            return VmmMapResult::PageTableOutOfMemory;
        }
        newly_allocated[level] = table_phys;
        PageTable* new_table = reinterpret_cast<PageTable*>(table_phys + pmm_hhdm_offset());
        for (size_t i = 0; i < 512; ++i) new_table->entries[i] = 0;
        tables[level + 1] = new_table;
    }

    uint64_t& leaf = tables[3]->entries[PT_INDEX(virt)];
    if (leaf != 0) {
        const bool leaf_present = (leaf & VMM_PRESENT) != 0;
        for (size_t rollback = 3; rollback > 0; --rollback) {
            if (newly_allocated[rollback - 1]) pmm_free(newly_allocated[rollback - 1], 0);
        }
        unlock_vmm(lock_flags);
        return leaf_present ? VmmMapResult::MappingExists : VmmMapResult::UnsupportedMapping;
    }

    // Publish newly built levels only after every allocation and validation succeeds.
    for (size_t level = 0; level < 3; ++level) {
        if (newly_allocated[level]) {
            uint64_t parent_flags = VMM_PRESENT | VMM_WRITE;
            if (flags & VMM_USER) parent_flags |= VMM_USER;
            tables[level]->entries[indices[level]] = newly_allocated[level] | parent_flags;
        }
    }
    leaf = phys | flags | owner;

    invalidate_page_all_cpus(virt);
    unlock_vmm(lock_flags);
    return VmmMapResult::Success;
}

VmmMapResult vmm_map_page(uintptr_t root, uintptr_t virt, uintptr_t phys, uint64_t flags) {
    return map_page_with_owner(root, virt, phys, flags, PTE_OWNER_BORROWED);
}

VmmMapResult vmm_map_owned_page(uintptr_t root, uintptr_t virt, uintptr_t phys, uint64_t flags) {
    if (!pmm_is_allocated_page(phys)) return VmmMapResult::InvalidPhysicalAddress;
    return map_page_with_owner(root, virt, phys, flags, PTE_OWNER_OWNED);
}

VmmMapResult vmm_map(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    return vmm_map_page(read_cr3() & PTE_ADDR_MASK, virt, phys, flags);
}

VmmMapResult vmm_map_mmio_page(uintptr_t virt, uintptr_t phys) {
    uintptr_t root_phys = read_cr3() & PTE_ADDR_MASK;
    if (root_phys != s_kernel_pml4 || !s_kernel_pml4 || (virt & (PAGE_SIZE - 1)) ||
        !is_canonical(virt) || is_user_address(virt)) {
        return VmmMapResult::InvalidVirtualAddress;
    }
    if (!phys || (phys & (PAGE_SIZE - 1)) || (phys & ~PTE_ADDR_MASK)) {
        return VmmMapResult::InvalidPhysicalAddress;
    }

    uint64_t lock_flags = lock_vmm();
    PageTable* pml4 = reinterpret_cast<PageTable*>(root_phys + pmm_hhdm_offset());
    uint64_t& l4e = pml4->entries[PML4_INDEX(virt)];
    if (!(l4e & VMM_PRESENT)) {
        unlock_vmm(lock_flags);
        return map_page_with_owner(root_phys, virt, phys, VMM_MMIO, PTE_OWNER_MMIO);
    }
    if (l4e & PTE_PS) {
        unlock_vmm(lock_flags);
        return VmmMapResult::UnsupportedMapping;
    }
    PageTable* pdpt = reinterpret_cast<PageTable*>((l4e & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t& l3e = pdpt->entries[PDPT_INDEX(virt)];
    if (!(l3e & VMM_PRESENT)) {
        unlock_vmm(lock_flags);
        return map_page_with_owner(root_phys, virt, phys, VMM_MMIO, PTE_OWNER_MMIO);
    }

    if (l3e & PTE_PS) {
        uintptr_t mapped_phys = (l3e & PDPTE_1G_ADDR_MASK) | (virt & PAGE_SIZE_1G_MASK);
        if (mapped_phys != phys) {
            unlock_vmm(lock_flags);
            return VmmMapResult::MappingExists;
        }

        uintptr_t pd_phys = allocate_table_page();
        if (!pd_phys) {
            unlock_vmm(lock_flags);
            return VmmMapResult::PageTableOutOfMemory;
        }
        PageTable* pd = reinterpret_cast<PageTable*>(pd_phys + pmm_hhdm_offset());
        fill_pd_from_1g(pd, l3e);

        size_t pd_index = PD_INDEX(virt);
        uint64_t original_2m = pd->entries[pd_index];
        uintptr_t pt_phys = allocate_table_page();
        if (!pt_phys) {
            pmm_free_page(pd_phys);
            unlock_vmm(lock_flags);
            return VmmMapResult::PageTableOutOfMemory;
        }
        PageTable* pt = reinterpret_cast<PageTable*>(pt_phys + pmm_hhdm_offset());
        fill_pt_from_2m(pt, original_2m);
        uint64_t& target = pt->entries[PT_INDEX(virt)];
        target = (target & PTE_ADDR_MASK) | mmio_leaf_flags(target);
        pd->entries[pd_index] = pt_phys | table_pointer_flags(original_2m);

        // Commit only when both new levels are complete; the old 1 GiB entry
        // remains untouched if either allocation fails.
        l3e = pd_phys | table_pointer_flags(l3e);
        invalidate_page_all_cpus(virt);
        unlock_vmm(lock_flags);
        return VmmMapResult::Success;
    }

    PageTable* pd = reinterpret_cast<PageTable*>((l3e & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t& l2e = pd->entries[PD_INDEX(virt)];
    if (!(l2e & VMM_PRESENT)) {
        unlock_vmm(lock_flags);
        return map_page_with_owner(root_phys, virt, phys, VMM_MMIO, PTE_OWNER_MMIO);
    }
    if (l2e & PTE_PS) {
        uintptr_t mapped_phys = (l2e & PDE_2M_ADDR_MASK) | (virt & PAGE_SIZE_2M_MASK);
        if (mapped_phys != phys) {
            unlock_vmm(lock_flags);
            return VmmMapResult::MappingExists;
        }

        uintptr_t pt_phys = allocate_table_page();
        if (!pt_phys) {
            unlock_vmm(lock_flags);
            return VmmMapResult::PageTableOutOfMemory;
        }
        PageTable* pt = reinterpret_cast<PageTable*>(pt_phys + pmm_hhdm_offset());
        uint64_t original_2m = l2e;
        fill_pt_from_2m(pt, original_2m);
        uint64_t& target = pt->entries[PT_INDEX(virt)];
        target = (target & PTE_ADDR_MASK) | mmio_leaf_flags(target);
        l2e = pt_phys | table_pointer_flags(original_2m);
        invalidate_page_all_cpus(virt);
        unlock_vmm(lock_flags);
        return VmmMapResult::Success;
    }

    PageTable* pt = reinterpret_cast<PageTable*>((l2e & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t& leaf = pt->entries[PT_INDEX(virt)];
    if (!(leaf & VMM_PRESENT)) {
        unlock_vmm(lock_flags);
        return map_page_with_owner(root_phys, virt, phys, VMM_MMIO, PTE_OWNER_MMIO);
    }
    if ((leaf & PTE_ADDR_MASK) != phys) {
        unlock_vmm(lock_flags);
        return VmmMapResult::MappingExists;
    }

    uint64_t desired_flags = mmio_leaf_flags(leaf);
    if (desired_flags != (leaf & PTE_FLAGS_MASK)) {
        leaf = phys | desired_flags;
        invalidate_page_all_cpus(virt);
    }
    unlock_vmm(lock_flags);
    return VmmMapResult::Success;
}

uintptr_t vmm_create_address_space() {
    uint64_t lock_flags = lock_vmm();
    uintptr_t root = pmm_alloc_page();
    if (!root) { unlock_vmm(lock_flags); return 0; }
    PageTable* pt = reinterpret_cast<PageTable*>(root + pmm_hhdm_offset());
    for (size_t i = 0; i < 256; ++i) pt->entries[i] = 0;
    PageTable* kernel_pt = reinterpret_cast<PageTable*>(s_kernel_pml4 + pmm_hhdm_offset());
    for (size_t i = 256; i < 512; ++i) pt->entries[i] = kernel_pt->entries[i];
    if (!register_address_space(root)) {
        pmm_free_page(root);
        unlock_vmm(lock_flags);
        return 0;
    }
    unlock_vmm(lock_flags);
    return root;
}

VmmAddressSpaceResult vmm_switch_address_space(uintptr_t root) {
    uint64_t lock_flags = lock_vmm();
    if (!valid_root(root)) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::InvalidAddressSpace; }
    if (root != s_kernel_pml4) {
        PageTable* dst = reinterpret_cast<PageTable*>(root + pmm_hhdm_offset());
        PageTable* src = reinterpret_cast<PageTable*>(s_kernel_pml4 + pmm_hhdm_offset());
        for (size_t i = 256; i < 512; ++i) dst->entries[i] = src->entries[i];
    }
    write_cr3(root);
    unlock_vmm(lock_flags);
    return VmmAddressSpaceResult::Success;
}

VmmAddressSpaceResult vmm_retain_address_space(uintptr_t root) {
    uint64_t lock_flags = lock_vmm();
    AddressSpaceRecord* record = find_address_space(root);
    if (root == s_kernel_pml4) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::Success; }
    if (!record) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::InvalidAddressSpace; }
    if (record->references == UINT32_MAX) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::AddressSpaceInUse; }
    ++record->references;
    unlock_vmm(lock_flags);
    return VmmAddressSpaceResult::Success;
}

VmmAddressSpaceResult vmm_release_address_space(uintptr_t root) {
    uint64_t lock_flags = lock_vmm();
    AddressSpaceRecord* record = find_address_space(root);
    if (root == s_kernel_pml4) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::Success; }
    if (!record) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::InvalidAddressSpace; }
    if (!record->references) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::AddressSpaceInUse; }
    --record->references;
    unlock_vmm(lock_flags);
    return VmmAddressSpaceResult::Success;
}

uintptr_t vmm_get_kernel_pml4() {
    return s_kernel_pml4;
}

bool vmm_query_mapping(uintptr_t virt, VmmMappingInfo* info) {
    if (!info || !is_canonical(virt)) return false;
    uint64_t lock_flags = lock_vmm();
    uintptr_t active_pml4 = read_cr3() & PTE_ADDR_MASK;
    PageTable* pml4 = reinterpret_cast<PageTable*>(active_pml4 + pmm_hhdm_offset());
    uint64_t e4 = pml4->entries[PML4_INDEX(virt)];
    if (!(e4 & VMM_PRESENT) || (e4 & PTE_PS)) {
        unlock_vmm(lock_flags);
        return false;
    }
    PageTable* pdpt = reinterpret_cast<PageTable*>((e4 & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t e3 = pdpt->entries[PDPT_INDEX(virt)];
    if (!(e3 & VMM_PRESENT)) {
        unlock_vmm(lock_flags);
        return false;
    }
    if (e3 & PTE_PS) {
        info->physical_page = ((e3 & PDPTE_1G_ADDR_MASK) | (virt & PAGE_SIZE_1G_MASK)) &
                              ~(PAGE_SIZE - 1);
        info->flags = ((e3 & PTE_FLAGS_MASK) | (e3 & PDE_PAT_LARGE)) &
                      ~(PTE_OWNER_BORROWED | PTE_OWNER_OWNED | PTE_OWNER_MMIO);
        info->page_size = PAGE_SIZE_1G;
        info->ownership = VmmMappingInfo::Ownership::Borrowed;
        unlock_vmm(lock_flags);
        return true;
    }
    PageTable* pd = reinterpret_cast<PageTable*>((e3 & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t e2 = pd->entries[PD_INDEX(virt)];
    if (!(e2 & VMM_PRESENT)) {
        unlock_vmm(lock_flags);
        return false;
    }
    if (e2 & PTE_PS) {
        info->physical_page = ((e2 & PDE_2M_ADDR_MASK) | (virt & PAGE_SIZE_2M_MASK)) &
                              ~(PAGE_SIZE - 1);
        info->flags = ((e2 & PTE_FLAGS_MASK) | (e2 & PDE_PAT_LARGE)) &
                      ~(PTE_OWNER_BORROWED | PTE_OWNER_OWNED | PTE_OWNER_MMIO);
        info->page_size = PAGE_SIZE_2M;
        info->ownership = VmmMappingInfo::Ownership::Borrowed;
        unlock_vmm(lock_flags);
        return true;
    }
    PageTable* pt = reinterpret_cast<PageTable*>((e2 & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t e1 = pt->entries[PT_INDEX(virt)];
    if (!(e1 & VMM_PRESENT)) {
        unlock_vmm(lock_flags);
        return false;
    }
    info->physical_page = e1 & PTE_ADDR_MASK;
    info->flags = e1 & PTE_FLAGS_MASK & ~(PTE_OWNER_BORROWED | PTE_OWNER_OWNED | PTE_OWNER_MMIO);
    info->page_size = PAGE_SIZE;
    info->ownership = (e1 & PTE_OWNER_OWNED) ? VmmMappingInfo::Ownership::Owned :
                      (e1 & PTE_OWNER_MMIO) ? VmmMappingInfo::Ownership::Mmio :
                      VmmMappingInfo::Ownership::Borrowed;
    unlock_vmm(lock_flags);
    return true;
}

static bool table_empty(PageTable* table) {
    for (size_t i = 0; i < 512; ++i) if (table->entries[i] & VMM_PRESENT) return false;
    return true;
}

static VmmUnmapResult split_large_for_unmap(uintptr_t root, uintptr_t virt) {
    PageTable* pml4 = reinterpret_cast<PageTable*>(root + pmm_hhdm_offset());
    uint64_t e4 = pml4->entries[PML4_INDEX(virt)];
    if (!(e4 & VMM_PRESENT) || (e4 & PTE_PS)) return VmmUnmapResult::NotMapped;
    PageTable* pdpt = reinterpret_cast<PageTable*>((e4 & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t& e3 = pdpt->entries[PDPT_INDEX(virt)];
    if (!(e3 & VMM_PRESENT)) return VmmUnmapResult::NotMapped;
    if (e3 & PTE_PS) {
        if (e3 & PTE_OWNER_OWNED) return VmmUnmapResult::UnsupportedMapping;
        uintptr_t pd_phys = allocate_table_page();
        if (!pd_phys) return VmmUnmapResult::PageTableOutOfMemory;
        PageTable* pd = reinterpret_cast<PageTable*>(pd_phys + pmm_hhdm_offset());
        fill_pd_from_1g(pd, e3);
        uint64_t original = e3;
        size_t index = PD_INDEX(virt);
        uint64_t original_2m = pd->entries[index];
        uintptr_t pt_phys = allocate_table_page();
        if (!pt_phys) { pmm_free_page(pd_phys); return VmmUnmapResult::PageTableOutOfMemory; }
        PageTable* pt = reinterpret_cast<PageTable*>(pt_phys + pmm_hhdm_offset());
        fill_pt_from_2m(pt, original_2m);
        pd->entries[index] = pt_phys | table_pointer_flags(original_2m);
        e3 = pd_phys | table_pointer_flags(original);
        return VmmUnmapResult::Success;
    }
    PageTable* pd = reinterpret_cast<PageTable*>((e3 & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t& e2 = pd->entries[PD_INDEX(virt)];
    if (!(e2 & VMM_PRESENT)) return VmmUnmapResult::NotMapped;
    if (e2 & PTE_PS) {
        if (e2 & PTE_OWNER_OWNED) return VmmUnmapResult::UnsupportedMapping;
        uintptr_t pt_phys = allocate_table_page();
        if (!pt_phys) return VmmUnmapResult::PageTableOutOfMemory;
        PageTable* pt = reinterpret_cast<PageTable*>(pt_phys + pmm_hhdm_offset());
        fill_pt_from_2m(pt, e2);
        e2 = pt_phys | table_pointer_flags(e2);
    }
    return VmmUnmapResult::Success;
}

VmmUnmapResult vmm_unmap_page(uintptr_t root, uintptr_t virt) {
    if (!is_canonical(virt) || (virt & (PAGE_SIZE - 1))) return VmmUnmapResult::InvalidVirtualAddress;
    if (!root || (root & (PAGE_SIZE - 1))) return VmmUnmapResult::InvalidAddressSpace;
    bool user = is_user_address(virt);
    uint64_t lock_flags = lock_vmm();
    if (!valid_root(root) || (root == s_kernel_pml4) == user) {
        unlock_vmm(lock_flags);
        return VmmUnmapResult::InvalidAddressSpace;
    }
    PageTable* pml4 = reinterpret_cast<PageTable*>(root + pmm_hhdm_offset());
    uint64_t e4 = pml4->entries[PML4_INDEX(virt)];
    if (!(e4 & VMM_PRESENT)) { unlock_vmm(lock_flags); return VmmUnmapResult::NotMapped; }
    PageTable* pdpt = reinterpret_cast<PageTable*>((e4 & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t e3 = pdpt->entries[PDPT_INDEX(virt)];
    if (!(e3 & VMM_PRESENT)) { unlock_vmm(lock_flags); return VmmUnmapResult::NotMapped; }
    if (e3 & PTE_PS) {
        VmmUnmapResult split = split_large_for_unmap(root, virt);
        if (split != VmmUnmapResult::Success) { unlock_vmm(lock_flags); return split; }
        e3 = pdpt->entries[PDPT_INDEX(virt)];
    }
    PageTable* pd = reinterpret_cast<PageTable*>((e3 & PTE_ADDR_MASK) + pmm_hhdm_offset());
    uint64_t e2 = pd->entries[PD_INDEX(virt)];
    if (!(e2 & VMM_PRESENT)) { unlock_vmm(lock_flags); return VmmUnmapResult::NotMapped; }
    if (e2 & PTE_PS) {
        VmmUnmapResult split = split_large_for_unmap(root, virt);
        if (split != VmmUnmapResult::Success) { unlock_vmm(lock_flags); return split; }
        e3 = pdpt->entries[PDPT_INDEX(virt)];
        pd = reinterpret_cast<PageTable*>((e3 & PTE_ADDR_MASK) + pmm_hhdm_offset());
        e2 = pd->entries[PD_INDEX(virt)];
    }
    uintptr_t pt_phys = e2 & PTE_ADDR_MASK;
    PageTable* pt = reinterpret_cast<PageTable*>(pt_phys + pmm_hhdm_offset());
    uint64_t& leaf = pt->entries[PT_INDEX(virt)];
    if (!(leaf & VMM_PRESENT)) { unlock_vmm(lock_flags); return VmmUnmapResult::NotMapped; }
    uintptr_t frame = leaf & PTE_ADDR_MASK;
    bool owned = (leaf & PTE_OWNER_OWNED) != 0;
    leaf = 0;
    bool active = root == (read_cr3() & PTE_ADDR_MASK);
    bool shared_kernel_mapping = root == s_kernel_pml4 && !user;
    if (active || shared_kernel_mapping) invlpg(virt);
    if (!smp_tlb_invalidate_page(virt)) panic("TLB shootdown timed out during unmap");

    // Reclaim only lower-half tables belonging to a registered user root.
    if (user && table_empty(pt)) {
        pd->entries[PD_INDEX(virt)] = 0;
        pmm_free_page(pt_phys);
        if (table_empty(pd)) {
            uintptr_t pd_phys = e3 & PTE_ADDR_MASK;
            pdpt->entries[PDPT_INDEX(virt)] = 0;
            pmm_free_page(pd_phys);
            if (table_empty(pdpt)) {
                uintptr_t pdpt_phys = e4 & PTE_ADDR_MASK;
                pml4->entries[PML4_INDEX(virt)] = 0;
                pmm_free_page(pdpt_phys);
            }
        }
    }
    // Clear PTE and invalidate before releasing an owned data frame.
    if (owned) pmm_free_page(frame);
    unlock_vmm(lock_flags);
    return VmmUnmapResult::Success;
}

VmmUnmapResult vmm_unmap(uintptr_t virt) {
    return vmm_unmap_page(read_cr3() & PTE_ADDR_MASK, virt);
}

VmmAddressSpaceResult vmm_destroy_address_space(uintptr_t root) {
    uint64_t lock_flags = lock_vmm();
    if (root == s_kernel_pml4) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::KernelAddressSpace; }
    AddressSpaceRecord* record = find_address_space(root);
    if (!record) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::InvalidAddressSpace; }
    if ((read_cr3() & PTE_ADDR_MASK) == root || smp_address_space_active(root)) {
        unlock_vmm(lock_flags);
        return VmmAddressSpaceResult::ActiveAddressSpace;
    }
    if (record->references) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::AddressSpaceInUse; }

    PageTable* pml4 = reinterpret_cast<PageTable*>(root + pmm_hhdm_offset());
    // Validate the complete private lower-half tree before releasing anything.
    for (size_t i4 = 0; i4 < 256; ++i4) {
        uint64_t e4 = pml4->entries[i4];
        if (!(e4 & VMM_PRESENT)) continue;
        if (e4 & PTE_PS) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::InvalidAddressSpace; }
        PageTable* pdpt = reinterpret_cast<PageTable*>((e4 & PTE_ADDR_MASK) + pmm_hhdm_offset());
        for (size_t i3 = 0; i3 < 512; ++i3) {
            uint64_t e3 = pdpt->entries[i3];
            if (!(e3 & VMM_PRESENT)) continue;
            if ((e3 & PTE_PS) && (e3 & PTE_OWNER_OWNED)) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::InvalidAddressSpace; }
            if (e3 & PTE_PS) continue;
            PageTable* pd = reinterpret_cast<PageTable*>((e3 & PTE_ADDR_MASK) + pmm_hhdm_offset());
            for (size_t i2 = 0; i2 < 512; ++i2) {
                uint64_t e2 = pd->entries[i2];
                if (!(e2 & VMM_PRESENT)) continue;
                if ((e2 & PTE_PS) && (e2 & PTE_OWNER_OWNED)) { unlock_vmm(lock_flags); return VmmAddressSpaceResult::InvalidAddressSpace; }
                if (e2 & PTE_PS) continue;
                PageTable* pt = reinterpret_cast<PageTable*>((e2 & PTE_ADDR_MASK) + pmm_hhdm_offset());
                for (size_t i1 = 0; i1 < 512; ++i1)
                    if ((pt->entries[i1] & VMM_PRESENT) && (pt->entries[i1] & PTE_OWNER_OWNED) &&
                        !pmm_is_allocated_page(pt->entries[i1] & PTE_ADDR_MASK)) {
                        unlock_vmm(lock_flags); return VmmAddressSpaceResult::InvalidAddressSpace;
                    }
            }
        }
    }
    flush_address_spaces_all_cpus();
    for (size_t i4 = 0; i4 < 256; ++i4) {
        uint64_t e4 = pml4->entries[i4]; if (!(e4 & VMM_PRESENT)) continue;
        PageTable* pdpt = reinterpret_cast<PageTable*>((e4 & PTE_ADDR_MASK) + pmm_hhdm_offset());
        for (size_t i3 = 0; i3 < 512; ++i3) {
            uint64_t e3 = pdpt->entries[i3]; if (!(e3 & VMM_PRESENT) || (e3 & PTE_PS)) continue;
            PageTable* pd = reinterpret_cast<PageTable*>((e3 & PTE_ADDR_MASK) + pmm_hhdm_offset());
            for (size_t i2 = 0; i2 < 512; ++i2) {
                uint64_t e2 = pd->entries[i2]; if (!(e2 & VMM_PRESENT) || (e2 & PTE_PS)) continue;
                PageTable* pt = reinterpret_cast<PageTable*>((e2 & PTE_ADDR_MASK) + pmm_hhdm_offset());
                for (size_t i1 = 0; i1 < 512; ++i1) {
                    uint64_t e1 = pt->entries[i1];
                    if ((e1 & VMM_PRESENT) && (e1 & PTE_OWNER_OWNED)) pmm_free_page(e1 & PTE_ADDR_MASK);
                }
                pmm_free_page(e2 & PTE_ADDR_MASK);
            }
            pmm_free_page(e3 & PTE_ADDR_MASK);
        }
        pmm_free_page(e4 & PTE_ADDR_MASK);
    }
    if (!unregister_address_space(root)) {
        unlock_vmm(lock_flags);
        return VmmAddressSpaceResult::InvalidAddressSpace;
    }
    pmm_free_page(root);
    unlock_vmm(lock_flags);
    return VmmAddressSpaceResult::Success;
}

uintptr_t vmm_get_phys(uintptr_t virt) {
    VmmMappingInfo info = {};
    return vmm_query_mapping(virt, &info)
        ? info.physical_page | (virt & (PAGE_SIZE - 1))
        : 0;
}

// -------------------------------------------------------------------
// MODULE LIFECYCLE
// -------------------------------------------------------------------

bool VMM::start() {
    // 1. Allocate a new physical page for our kernel PML4
    s_kernel_pml4 = pmm_alloc_page();
    if (!s_kernel_pml4) return false; // OOM

    // 2. Zero out the new PML4
    PageTable* new_pml4 = reinterpret_cast<PageTable*>(s_kernel_pml4 + pmm_hhdm_offset());
    for (int i = 0; i < 512; i++) {
        new_pml4->entries[i] = 0;
    }

    // 3. Preserve Limine's higher-half mappings.
    //    Limine's PML4 is currently active in CR3.
    //    We copy entries 256 to 511 (the higher half) into our new PML4.
    uintptr_t current_cr3 = read_cr3();
    PageTable* limine_pml4 = reinterpret_cast<PageTable*>(current_cr3 + pmm_hhdm_offset());
    
    for (int i = 256; i < 512; i++) {
        new_pml4->entries[i] = limine_pml4->entries[i];
    }

    // 4. Load our new PML4 into CR3
    write_cr3(s_kernel_pml4);

    return true;
}

void VMM::stop() {
    // VMM cannot be stopped.
}

const char* VMM::get_name() const {
    return "Virtual Memory Manager (x86_64 Paging)";
}

// Register as the SECOND memory module (depends on PMM)
static const char* const kVmmDependencies[] = {"g_pmm"};
REGISTER_MODULE_REQUIRED(g_vmm, VMM, 2_mem_b, kVmmDependencies);
