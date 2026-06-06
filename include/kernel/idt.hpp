#pragma once

#include <stdint.h>
#include "module.hpp"

// IDT Entry for x86_64 (16 bytes)
struct IDTEntry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t ist;        // Interrupt Stack Table offset
    uint8_t attributes; // Type and attributes
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed));

// IDT Pointer structure (10 bytes) used by lidt
struct IDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// Interrupt frame passed by GCC when using __attribute__((interrupt))
struct InterruptFrame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

class IDT : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

private:
    void set_entry(int index, uint64_t isr, uint8_t flags);
    
    // We need 256 entries for a complete IDT
    IDTEntry entries[256];
    IDTPointer pointer;
};
