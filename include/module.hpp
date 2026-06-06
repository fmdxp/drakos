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
inline void operator delete(void*, size_t) noexcept {}
inline void operator delete[](void*, size_t) noexcept {}
// Also required to silence -Wsized-deallocation warnings
inline void operator delete(void*) noexcept {}
inline void operator delete[](void*) noexcept {}

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

