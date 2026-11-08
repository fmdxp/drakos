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
#include "kernel/initcall_registry.hpp"

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

    // Return a static explanation when start() fails. stop() must undo partial setup.
    virtual const char* failure_reason() const { return "start() returned false"; }
};

#define DRK_PHASE_1_core InitcallPhase::Core
#define DRK_PHASE_2_mem_a InitcallPhase::MemoryA
#define DRK_PHASE_2_mem_b InitcallPhase::MemoryB
#define DRK_PHASE_2_mem_c InitcallPhase::MemoryC
#define DRK_PHASE_3_acpi InitcallPhase::Firmware
#define DRK_PHASE_3_drv InitcallPhase::Driver
#define DRK_PHASE_4_dev InitcallPhase::Device
#define DRK_PHASE_5_usb InitcallPhase::Bus
#define DRK_PHASE_EXPAND(level) DRK_PHASE_##level
#define DRK_PHASE(level) DRK_PHASE_EXPAND(level)

#define REGISTER_MODULE_CONFIG(obj_name, class_type, level, required, deps, dep_count) \
    alignas(class_type) static uint8_t __storage_##obj_name[sizeof(class_type)]; \
    \
    class_type* obj_name = nullptr; \
    \
    static InitcallResult __init_##obj_name() { \
        obj_name = new (__storage_##obj_name) class_type(); \
        if (!obj_name->start()) { \
            const char* error = obj_name->failure_reason(); \
            obj_name->stop(); \
            obj_name = nullptr; \
            return {false, error}; \
        } \
        return {true, nullptr}; \
    } \
    \
    static const InitcallDescriptor __initcall_##obj_name \
    __attribute__((section(".initcalls." #level), used, aligned(8))) = { \
        #obj_name, DRK_PHASE(level), required, deps, dep_count, __init_##obj_name \
    };

#define REGISTER_MODULE(obj_name, class_type, level) \
    REGISTER_MODULE_CONFIG(obj_name, class_type, level, InitcallRequirement::Optional, nullptr, 0)

#define REGISTER_MODULE_OPTIONAL(obj_name, class_type, level, deps_array) \
    REGISTER_MODULE_CONFIG(obj_name, class_type, level, InitcallRequirement::Optional, \
        deps_array, sizeof(deps_array) / sizeof((deps_array)[0]))

#define REGISTER_MODULE_REQUIRED(obj_name, class_type, level, deps_array) \
    REGISTER_MODULE_CONFIG(obj_name, class_type, level, InitcallRequirement::Mandatory, \
        deps_array, sizeof(deps_array) / sizeof((deps_array)[0]))

#define REGISTER_MODULE_REQUIRED_NO_DEPS(obj_name, class_type, level) \
    REGISTER_MODULE_CONFIG(obj_name, class_type, level, InitcallRequirement::Mandatory, nullptr, 0)

// ---------------------------------------------------------
// SYSTEM INITIALIZATION
// ---------------------------------------------------------
// Call this function once during OS boot to execute all initcalls
InitcallSummary system_init_modules();

