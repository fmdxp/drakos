#pragma once

#include <stddef.h>
#include "module.hpp"

// -------------------------------------------------------------------
// HEAP PUBLIC API (SLUB Allocator)
// -------------------------------------------------------------------

// Allocate 'size' bytes of memory. Returns nullptr if out of memory.
void* kmalloc(size_t size);

// Free memory previously allocated by kmalloc.
void  kfree(void* ptr);

// -------------------------------------------------------------------
// C++ OPERATOR NEW/DELETE OVERRIDES
// These allow us to use normal 'new' and 'delete' for classes.
// -------------------------------------------------------------------
void* operator new(size_t size);
void* operator new[](size_t size);
void  operator delete(void* p) noexcept;
void  operator delete[](void* p) noexcept;

// -------------------------------------------------------------------
// HEAP MODULE
// Registered at level 2_mem_c (runs after VMM)
// -------------------------------------------------------------------
class Heap : public KernelModule {
public:
    bool        start()    override;
    void        stop()     override;
    const char* get_name() const override;
};

extern Heap* g_heap;
