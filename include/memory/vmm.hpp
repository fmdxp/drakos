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


#pragma once

#include <stdint.h>
#include <stddef.h>
#include "module.hpp"

// -------------------------------------------------------------------
// PAGE TABLE FLAGS (x86_64)
// These bits go into each 64-bit page table entry.
// -------------------------------------------------------------------
#define VMM_PRESENT  (1ULL << 0)   // Page is mapped and accessible
#define VMM_WRITE    (1ULL << 1)   // Page is writable
#define VMM_USER     (1ULL << 2)   // Accessible from user space (Ring 3)
#define VMM_CACHE_DISABLE ((1ULL << 3) | (1ULL << 4)) // Page-level Write-Through and Cache Disable
#define VMM_NX       (1ULL << 63)  // No-Execute (data pages should have this)

// Common combinations
#define VMM_KERNEL_RW  (VMM_PRESENT | VMM_WRITE)
#define VMM_KERNEL_RO  (VMM_PRESENT)
#define VMM_MMIO       (VMM_PRESENT | VMM_WRITE | VMM_CACHE_DISABLE)

// -------------------------------------------------------------------
// VMM PUBLIC API
// -------------------------------------------------------------------

enum class VmmMapResult : uint8_t {
    Success,
    InvalidVirtualAddress,
    InvalidPhysicalAddress,
    MappingExists,
    PageTableOutOfMemory,
    InvalidAddressSpace,
    UnsupportedMapping,
    FrameAlreadyOwned,
};

struct VmmMappingInfo {
    uintptr_t physical_page;
    uint64_t flags;
    size_t page_size;
    enum class Ownership : uint8_t { Borrowed, Owned, Mmio } ownership;
};

enum class VmmUnmapResult : uint8_t {
    Success,
    NotMapped,
    InvalidVirtualAddress,
    InvalidAddressSpace,
    PageTableOutOfMemory,
    UnsupportedMapping,
};

enum class VmmAddressSpaceResult : uint8_t {
    Success,
    InvalidAddressSpace,
    KernelAddressSpace,
    ActiveAddressSpace,
    AddressSpaceInUse,
    OutOfMemory,
};

// Map a virtual address to a physical address with the given flags in the CURRENT address space.
// Creates intermediate page table levels as needed (uses PMM internally).
VmmMapResult vmm_map(uintptr_t virt, uintptr_t phys, uint64_t flags);

// Map an MMIO page in the active kernel address space. If the HHDM already
// maps the same physical page, this explicitly applies MMIO cache attributes;
// a different existing physical mapping is never replaced.
VmmMapResult vmm_map_mmio_page(uintptr_t virt, uintptr_t phys);

// Query the active address space, including existing 1 GiB and 2 MiB leaves.
bool vmm_query_mapping(uintptr_t virt, VmmMappingInfo* info);

// Map a virtual address to a physical address in a SPECIFIC address space (PML4).
VmmMapResult vmm_map_page(uintptr_t pml4_phys, uintptr_t virt, uintptr_t phys, uint64_t flags);

// Explicitly map an exclusively owned PMM order-0 frame. Existing vmm_map*
// APIs continue to create borrowed mappings and never transfer frame ownership.
// A frame already tagged owned in any address space is rejected.
VmmMapResult vmm_map_owned_page(uintptr_t pml4_phys, uintptr_t virt,
                                uintptr_t phys, uint64_t flags);

// Create a new address space (returns physical address of new PML4)
uintptr_t vmm_create_address_space();

// Switch to a new address space
VmmAddressSpaceResult vmm_switch_address_space(uintptr_t pml4_phys);

// Address spaces created by VMM are reference-counted while threads can run
// them. Destruction succeeds only for an inactive root with no references.
VmmAddressSpaceResult vmm_retain_address_space(uintptr_t pml4_phys);
VmmAddressSpaceResult vmm_release_address_space(uintptr_t pml4_phys);
VmmAddressSpaceResult vmm_destroy_address_space(uintptr_t pml4_phys);

// Return the kernel master PML4 physical address
uintptr_t vmm_get_kernel_pml4();

// Remove the mapping for a virtual address and flush the TLB entry.
VmmUnmapResult vmm_unmap(uintptr_t virt);
VmmUnmapResult vmm_unmap_page(uintptr_t pml4_phys, uintptr_t virt);

// Walk the current page tables and return the physical address mapped
// at 'virt'. Returns 0 if not mapped.
uintptr_t vmm_get_phys(uintptr_t virt);

#ifdef DRAKOS_HOST_TEST
void vmm_test_set_cr3(uintptr_t value);
uintptr_t vmm_test_last_invalidation();
void vmm_test_fail_page_table_allocation_after(int successful_allocations);
void vmm_test_fail_registry_allocation_after(int successful_allocations);
#endif

// -------------------------------------------------------------------
// VMM MODULE
// Registered at level 2_mem_b (runs after PMM, before Heap)
// -------------------------------------------------------------------
class VMM : public KernelModule {
public:
    bool        start()    override;
    void        stop()     override;
    const char* get_name() const override;
};

extern VMM* g_vmm;
