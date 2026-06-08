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

// PLACEMENT NEW OPERATOR
// Allows calling C++ constructors on statically allocated
// memory without needing a Heap/Allocator.

inline void* operator new(size_t, void* p) noexcept {
    return p;
}
inline void* operator new[](size_t, void* p) noexcept {
    return p;
}

// OPERATOR DELETE (no-op)
// Modules are statically allocated and never freed. However, the C++ compiler
// still emits a reference to operator delete when a class has a virtual
// destructor. We define it here as a no-op to satisfy the linker without
// pulling in the standard library.

// BASE MODULE INTERFACE
class KernelModule {
public:
    virtual ~KernelModule() = default;

    // Called during initialization. Return false if it fails.
    virtual bool start() = 0;

    // Called to logically detach the module from the OS.
    virtual void stop() = 0;

    // For debugging/logging
    virtual const char* get_name() const = 0;
};

// INITCALL REGISTRY
typedef bool (*initcall_t)();

#define REGISTER_MODULE(obj_name, class_type, level) \
    /* 1. Allocate a static byte array for the object */ \
    static uint8_t __storage_##obj_name[sizeof(class_type)]; \
    \
    /* 2. Create the global pointer */ \
    class_type* obj_name = nullptr; \
    \
    /* 3. The init function that uses placement new */ \
    static bool __init_##obj_name() { \
        obj_name = new (__storage_##obj_name) class_type(); \
        if (!obj_name->start()) { \
            obj_name->stop(); \
            return false; \
        } \
        return true; \
    } \
    \
    /* 4. Put the function pointer in the specific linker section */ \
    static initcall_t __initcall_##obj_name \
    __attribute__((section(".initcalls." #level), used)) = __init_##obj_name;

// ---------------------------------------------------------
// SYSTEM INITIALIZATION
// ---------------------------------------------------------
// Call this function once during OS boot to execute all initcalls
void system_init_modules();

