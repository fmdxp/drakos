#pragma once

#include <stdint.h>
#include "module.hpp"

// GDT Entry structure (8 bytes)
struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t flags; // High 4 bits (flags) + Low 4 bits (limit_high)
    uint8_t base_high;
} __attribute__((packed));

// GDT Pointer structure (10 bytes) used by lgdt
struct GDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

class GDT : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

private:
    void set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags);
    
    // We need 5 entries: Null, Kernel Code, Kernel Data, User Data, User Code
    GDTEntry entries[5];
    GDTPointer pointer;
};
