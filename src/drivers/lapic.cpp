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
#include "msr.hpp"

bool LocalAPIC::start() {
    // We depend on ACPI to find the physical address
    if (!g_acpi || g_acpi->get_lapic_phys() == 0) {
        return false;
    }

    // Access via Higher Half Direct Map (HHDM)
    m_lapic_virt = g_acpi->get_lapic_phys() + pmm_hhdm_offset();
    
    // CRITICAL: Force VMM_MMIO (Cache Disable) on the LAPIC page! 
    // Otherwise CPU caches writes to SVR and the APIC never enables.
    if (vmm_map_mmio_page(m_lapic_virt & ~0xFFFULL,
                          g_acpi->get_lapic_phys() & ~0xFFFULL) != VmmMapResult::Success) return false;

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
void LocalAPIC::initialize_current_cpu() {
    uint64_t apic_base = rdmsr(0x1B);
    apic_base |= (1ULL << 11);
    apic_base &= ~(1ULL << 10); // The current driver uses xAPIC MMIO mode.
    wrmsr(0x1B, apic_base);
    write(LAPIC_TPR, 0);
    write(LAPIC_SVR, read(LAPIC_SVR) | 0x100 | 0xFF);
    write(LAPIC_LVT_TMR, 1U << 16); // APs service shootdowns, not scheduler ticks.
}

bool LocalAPIC::send_ipi(uint32_t apic_id, uint8_t vector) {
    if (apic_id > 0xFF) return false;
    for (uint32_t spin = 0; spin < 1000000; ++spin) {
        if (!(read(LAPIC_ICR_LOW) & (1U << 12))) break;
        if (spin == 999999) return false;
        asm volatile("pause");
    }
    write(LAPIC_ICR_HIGH, apic_id << 24);
    write(LAPIC_ICR_LOW, vector);
    for (uint32_t spin = 0; spin < 1000000; ++spin) {
        if (!(read(LAPIC_ICR_LOW) & (1U << 12))) return true;
        asm volatile("pause");
    }
    return false;
}

bool lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    return g_lapic && g_lapic->send_ipi(apic_id, vector);
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
static const char* const kLapicDependencies[] = {"g_acpi", "g_vmm"};
REGISTER_MODULE_OPTIONAL(g_lapic, LocalAPIC, 3_drv, kLapicDependencies);
