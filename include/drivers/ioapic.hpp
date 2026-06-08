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
