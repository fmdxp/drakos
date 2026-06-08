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

// -------------------------------------------------------------------
// X86_64 PAGING STRUCTURES
// -------------------------------------------------------------------

// Address translation masks
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_FLAGS_MASK 0xFFF0000000000FFFULL

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

// -------------------------------------------------------------------
// INTERNAL HELPERS
// -------------------------------------------------------------------

// Read CR3 (current PML4 physical address)
static inline uintptr_t read_cr3() {
    uintptr_t val;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

// Write CR3 (load a new PML4)
static inline void write_cr3(uintptr_t val) {
    asm volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}

// Invalidate a single TLB entry
static inline void invlpg(uintptr_t virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

// Helper to get the next level page table, allocating it if needed.
// Returns the physical address of the next table, or 0 on failure.
static uintptr_t get_next_level(uintptr_t table_phys, uint32_t index, bool allocate) {
    PageTable* table = reinterpret_cast<PageTable*>(table_phys + pmm_hhdm_offset());
    uint64_t entry = table->entries[index];

    if (entry & VMM_PRESENT) {
        return entry & PTE_ADDR_MASK;
    }

    if (!allocate) return 0;

    // Allocate a new page table
    uintptr_t new_table_phys = pmm_alloc_page();
    if (!new_table_phys) return 0; // OOM

    // Zero it out (via HHDM)
    PageTable* new_table = reinterpret_cast<PageTable*>(new_table_phys + pmm_hhdm_offset());
    for (int i = 0; i < 512; i++) new_table->entries[i] = 0;

    // Link it in the current table (Present, Writable, User accessible)
    // We set User accessible here so we don't restrict lower levels.
    // Real protection is applied at the final PT level.
    table->entries[index] = new_table_phys | VMM_PRESENT | VMM_WRITE | VMM_USER;

    return new_table_phys;
}

// -------------------------------------------------------------------
// PUBLIC API IMPLEMENTATION
// -------------------------------------------------------------------

void vmm_map(uintptr_t virt, uintptr_t phys, uint64_t flags) {
    // Get PDPT from PML4, allocate if missing
    uintptr_t pdpt = get_next_level(s_kernel_pml4, PML4_INDEX(virt), true);
    if (!pdpt) return; // OOM

    // Get PD from PDPT, allocate if missing
    uintptr_t pd = get_next_level(pdpt, PDPT_INDEX(virt), true);
    if (!pd) return; // OOM

    // Get PT from PD, allocate if missing
    uintptr_t pt_phys = get_next_level(pd, PD_INDEX(virt), true);
    if (!pt_phys) return; // OOM

    // Set the entry in the PT
    PageTable* pt = reinterpret_cast<PageTable*>(pt_phys + pmm_hhdm_offset());
    pt->entries[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags;

    // Flush the TLB entry for this virtual address
    invlpg(virt);
}

void vmm_unmap(uintptr_t virt) {
    // Walk down the page tables without allocating
    uintptr_t pdpt = get_next_level(s_kernel_pml4, PML4_INDEX(virt), false);
    if (!pdpt) return;

    uintptr_t pd = get_next_level(pdpt, PDPT_INDEX(virt), false);
    if (!pd) return;

    uintptr_t pt_phys = get_next_level(pd, PD_INDEX(virt), false);
    if (!pt_phys) return;

    // Clear the entry in the final PT
    PageTable* pt = reinterpret_cast<PageTable*>(pt_phys + pmm_hhdm_offset());
    pt->entries[PT_INDEX(virt)] = 0;

    // Flush the TLB entry
    invlpg(virt);
}

uintptr_t vmm_get_phys(uintptr_t virt) {
    // Walk down the page tables without allocating
    uintptr_t pdpt = get_next_level(s_kernel_pml4, PML4_INDEX(virt), false);
    if (!pdpt) return 0;

    uintptr_t pd = get_next_level(pdpt, PDPT_INDEX(virt), false);
    if (!pd) return 0;

    uintptr_t pt_phys = get_next_level(pd, PD_INDEX(virt), false);
    if (!pt_phys) return 0;

    // Check if the final entry is present
    PageTable* pt = reinterpret_cast<PageTable*>(pt_phys + pmm_hhdm_offset());
    uint64_t entry = pt->entries[PT_INDEX(virt)];
    
    if (entry & VMM_PRESENT) {
        return entry & PTE_ADDR_MASK;
    }
    return 0;
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
REGISTER_MODULE(g_vmm, VMM, 2_mem_b);
