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


#include "lapic.hpp"
#include "acpi.hpp"
#include "pmm.hpp" // For HHDM offset
#include "vmm.hpp" // For VMM_MMIO

bool LocalAPIC::start() {
    // We depend on ACPI to find the physical address
    if (!g_acpi || g_acpi->get_lapic_phys() == 0) {
        return false;
    }

    // Access via Higher Half Direct Map (HHDM)
    m_lapic_virt = g_acpi->get_lapic_phys() + pmm_hhdm_offset();
    
    // CRITICAL: Force VMM_MMIO (Cache Disable) on the LAPIC page! 
    // Otherwise CPU caches writes to SVR and the APIC never enables.
    vmm_map(m_lapic_virt & ~0xFFFULL, g_acpi->get_lapic_phys() & ~0xFFFULL, VMM_MMIO);

    // Enable the Local APIC by setting the 8th bit in the Spurious Interrupt Vector Register
    // We also map the spurious interrupt to vector 0xFF.
    uint32_t svr = read(LAPIC_SVR);
    write(LAPIC_SVR, svr | 0x100 | 0xFF);

    // --- APIC Timer Initialization ---
    // Divider 16
    write(LAPIC_TMRDIV, 0x03);
    // Timer vector 32, Periodic mode (bit 17)
    write(LAPIC_LVT_TMR, 32 | 0x20000);
    // Initial count (arbitrary delay for now, typically calibrated via PIT/ACPI)
    write(LAPIC_TMRINITCNT, 1000000);

    return true;
}

void LocalAPIC::stop() {
    // Disable APIC (clear 8th bit of SVR)
    if (m_lapic_virt) {
        write(LAPIC_LVT_TMR, 0x10000); // Mask timer
        uint32_t svr = read(LAPIC_SVR);
        write(LAPIC_SVR, svr & ~0x100);
    }
}

const char* LocalAPIC::get_name() const {
    return "Local APIC";
}

void lapic_eoi() {
    if (g_lapic) {
        g_lapic->eoi();
    }
}

void LocalAPIC::eoi() {
    // Writing any value to the EOI register acknowledges the interrupt
    write(LAPIC_EOI, 0);
}

uint32_t LocalAPIC::read(uint32_t reg) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(m_lapic_virt + reg);
    return *ptr;
}

void LocalAPIC::write(uint32_t reg, uint32_t value) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(m_lapic_virt + reg);
    *ptr = value;
}

// Register as a driver, after ACPI.
REGISTER_MODULE(g_lapic, LocalAPIC, 3_drv);
