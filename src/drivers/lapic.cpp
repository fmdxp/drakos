#include "lapic.hpp"
#include "acpi.hpp"
#include "pmm.hpp" // For HHDM offset

bool LocalAPIC::start() {
    // We depend on ACPI to find the physical address
    if (!g_acpi || g_acpi->get_lapic_phys() == 0) {
        return false;
    }

    // Access via Higher Half Direct Map (HHDM)
    m_lapic_virt = g_acpi->get_lapic_phys() + pmm_hhdm_offset();

    // Enable the Local APIC by setting the 8th bit in the Spurious Interrupt Vector Register
    // We also map the spurious interrupt to vector 0xFF.
    uint32_t svr = read(LAPIC_SVR);
    write(LAPIC_SVR, svr | 0x100 | 0xFF);

    return true;
}

void LocalAPIC::stop() {
    // Disable APIC (clear 8th bit of SVR)
    if (m_lapic_virt) {
        uint32_t svr = read(LAPIC_SVR);
        write(LAPIC_SVR, svr & ~0x100);
    }
}

const char* LocalAPIC::get_name() const {
    return "Local APIC";
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
