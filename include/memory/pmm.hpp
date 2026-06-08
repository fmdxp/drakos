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
// CONSTANTS
// -------------------------------------------------------------------

#define PAGE_SIZE  4096ULL
#define PAGE_SHIFT 12
#define MAX_ORDER  10     // Max block = 2^10 pages = 4MB

// -------------------------------------------------------------------
// PMM PUBLIC API
// These free functions are called by VMM, Heap, and anyone else
// who needs physical memory.
// -------------------------------------------------------------------

// Allocate 2^order contiguous physical pages.
// Returns the physical address of the block, or 0 on failure.
uintptr_t pmm_alloc(uint32_t order);

// Free a block of 2^order pages at the given physical address.
// The buddy system will attempt to merge it with its buddy.
void pmm_free(uintptr_t phys_addr, uint32_t order);

// Shorthand helpers for single pages (order 0)
inline uintptr_t pmm_alloc_page()              { return pmm_alloc(0); }
inline void      pmm_free_page(uintptr_t addr) { pmm_free(addr, 0); }

// Returns the HHDM offset Limine set up for us.
// To turn a physical address into a virtual one: virt = phys + pmm_hhdm_offset()
uint64_t pmm_hhdm_offset();

// -------------------------------------------------------------------
// PMM MODULE
// Registered at level 2_mem_a (first memory module to run)
// -------------------------------------------------------------------
class PMM : public KernelModule {
public:
    bool        start()    override;
    void        stop()     override;
    const char* get_name() const override;
};

extern PMM* g_pmm;
