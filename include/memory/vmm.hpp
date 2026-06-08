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
#define VMM_NX       (1ULL << 63)  // No-Execute (data pages should have this)

// Common combinations
#define VMM_KERNEL_RW  (VMM_PRESENT | VMM_WRITE)
#define VMM_KERNEL_RO  (VMM_PRESENT)

// -------------------------------------------------------------------
// VMM PUBLIC API
// -------------------------------------------------------------------

// Map a virtual address to a physical address with the given flags.
// Creates intermediate page table levels as needed (uses PMM internally).
void vmm_map(uintptr_t virt, uintptr_t phys, uint64_t flags);

// Remove the mapping for a virtual address and flush the TLB entry.
void vmm_unmap(uintptr_t virt);

// Walk the current page tables and return the physical address mapped
// at 'virt'. Returns 0 if not mapped.
uintptr_t vmm_get_phys(uintptr_t virt);

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
