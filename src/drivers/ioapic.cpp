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


#include "ioapic.hpp"
#include "acpi.hpp"
#include "pmm.hpp"
#include "vmm.hpp"

// Utility functions for x86 in/out instructions
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

bool IOAPIC::start() {
    if (!g_acpi || g_acpi->get_ioapic_phys() == 0) {
        return false;
    }

    // Access via HHDM
    m_ioapic_virt = g_acpi->get_ioapic_phys() + pmm_hhdm_offset();
    vmm_map(m_ioapic_virt & ~0xFFFULL, g_acpi->get_ioapic_phys() & ~0xFFFULL, VMM_MMIO);

    // Disable the legacy 8259 PIC
    disable_legacy_pic();

    return true;
}

void IOAPIC::stop() {
}

const char* IOAPIC::get_name() const {
    return "I/O APIC";
}

void IOAPIC::disable_legacy_pic() {
    // Legacy PIC initialization sequence
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    // Set offsets (doesn't matter since we disable it, but good practice)
    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    // Cascade setup
    outb(0x21, 4);
    outb(0xA1, 2);

    // 8086 mode
    outb(0x21, 1);
    outb(0xA1, 1);

    // Mask all interrupts
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

uint32_t IOAPIC::read(uint32_t reg) {
    volatile uint32_t* ioregsel = reinterpret_cast<volatile uint32_t*>(m_ioapic_virt);
    volatile uint32_t* iowin = reinterpret_cast<volatile uint32_t*>(m_ioapic_virt + 0x10);
    
    *ioregsel = reg;
    return *iowin;
}

void IOAPIC::write(uint32_t reg, uint32_t value) {
    volatile uint32_t* ioregsel = reinterpret_cast<volatile uint32_t*>(m_ioapic_virt);
    volatile uint32_t* iowin = reinterpret_cast<volatile uint32_t*>(m_ioapic_virt + 0x10);
    
    *ioregsel = reg;
    *iowin = value;
}

void IOAPIC::set_entry(uint8_t irq, uint8_t vector) {
    // Redirection table starts at register 0x10
    // Each entry is 64 bits (2 registers: low and high)
    uint32_t low_index = IOAPIC_REDTBL + irq * 2;
    uint32_t high_index = IOAPIC_REDTBL + irq * 2 + 1;

    // Write low 32 bits (vector, delivery mode, etc.)
    // For a standard IRQ, we just put the vector and leave flags at 0
    // (Fixed delivery, active high, edge triggered, unmasked)
    write(low_index, vector);

    // Write high 32 bits (destination APIC ID, 0 for bootstrap processor)
    write(high_index, 0);
}

// Register as a driver
REGISTER_MODULE(g_ioapic, IOAPIC, 3_drv);
