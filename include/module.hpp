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

