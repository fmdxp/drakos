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
