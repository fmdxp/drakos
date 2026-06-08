#pragma once

#include <stdint.h>
#include "module.hpp"

// I/O APIC Register Offsets
#define IOAPIC_ID               0x00
#define IOAPIC_VER              0x01
#define IOAPIC_ARB              0x02
#define IOAPIC_REDTBL           0x10

class IOAPIC : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

    // Route an IRQ (0-23) to a specific IDT vector
    void set_entry(uint8_t irq, uint8_t vector);

    // Read a register from the I/O APIC
    uint32_t read(uint32_t reg);

    // Write a register to the I/O APIC
    void write(uint32_t reg, uint32_t value);

private:
    uintptr_t m_ioapic_virt = 0;
    
    // Disables the legacy 8259 PIC
    void disable_legacy_pic();
};

extern IOAPIC* g_ioapic;
