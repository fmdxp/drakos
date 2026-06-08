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
