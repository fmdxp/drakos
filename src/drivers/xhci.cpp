#include "xhci.hpp"
#include "pci.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"

// Capability Registers Offsets
#define XHCI_CAPLENGTH      0x00
#define XHCI_HCIVERSION     0x02
#define XHCI_HCSPARAMS1     0x04

// Operational Registers Offsets (added to CAPLENGTH)
#define XHCI_USBCMD         0x00
#define XHCI_USBSTS         0x04
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14
#define XHCI_CRCR           0x18
#define XHCI_DCBAAP         0x30
#define XHCI_CONFIG         0x38

// USBCMD bits
#define USBCMD_RS           (1 << 0) // Run/Stop
#define USBCMD_HCRST        (1 << 1) // Host Controller Reset

// USBSTS bits
#define USBSTS_HCH          (1 << 0) // HC Halted
#define USBSTS_CNR          (1 << 11) // Controller Not Ready

uint32_t XHCI::read32(uint32_t offset) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(m_mmio_base + offset);
    return *ptr;
}

void XHCI::write32(uint32_t offset, uint32_t value) {
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(m_mmio_base + offset);
    *ptr = value;
}

void XHCI::write64(uint32_t offset, uint64_t value) {
    volatile uint64_t* ptr = reinterpret_cast<volatile uint64_t*>(m_mmio_base + offset);
    *ptr = value;
}

uint64_t XHCI::read64(uint32_t offset) {
    volatile uint64_t* ptr = reinterpret_cast<volatile uint64_t*>(m_mmio_base + offset);
    return *ptr;
}

bool XHCI::reset_controller() {
    uint32_t op_base = m_cap_length;

    // 1. Ensure controller is stopped
    uint32_t cmd = read32(op_base + XHCI_USBCMD);
    cmd &= ~USBCMD_RS;
    write32(op_base + XHCI_USBCMD, cmd);
    
    // Wait until HC is halted
    while ((read32(op_base + XHCI_USBSTS) & USBSTS_HCH) == 0) {
        // In a real OS, we'd sleep or yield. For now, spin.
    }
    
    // 2. Send Reset
    cmd = read32(op_base + XHCI_USBCMD);
    cmd |= USBCMD_HCRST;
    write32(op_base + XHCI_USBCMD, cmd);
    
    // Wait until reset clears
    while (read32(op_base + XHCI_USBCMD) & USBCMD_HCRST) {
        // Spin
    }
    
    // Wait until Controller Not Ready clears
    while (read32(op_base + XHCI_USBSTS) & USBSTS_CNR) {
        // Spin
    }

    if (g_vga) g_vga->write("xHCI: Controller Reset Complete.\n");
    return true;
}

bool XHCI::initialize_data_structures() {
    // 1. Set max device slots enabled (CONFIG register)
    uint32_t op_base = m_cap_length;
    write32(op_base + XHCI_CONFIG, m_max_slots);

    // 2. Allocate DCBAA (Device Context Base Address Array)
    // Must be 64-byte aligned. pmm_alloc returns 4096-byte aligned.
    m_dcbaa_phys = pmm_alloc(1);
    if (!m_dcbaa_phys) return false;

    // Zero out the DCBAA
    uint64_t* dcbaa_virt = reinterpret_cast<uint64_t*>(m_dcbaa_phys + pmm_hhdm_offset());
    for (int i = 0; i < 256; i++) dcbaa_virt[i] = 0;

    // Pass physical address to controller
    write64(op_base + XHCI_DCBAAP, m_dcbaa_phys);

    // 3. Allocate Command Ring (1 page is enough for many commands)
    // Must be 64-byte aligned.
    m_cmd_ring_phys = pmm_alloc(1);
    if (!m_cmd_ring_phys) return false;

    // Zero out the Command Ring
    uint64_t* cmd_ring_virt = reinterpret_cast<uint64_t*>(m_cmd_ring_phys + pmm_hhdm_offset());
    for (int i = 0; i < 512; i++) cmd_ring_virt[i] = 0;

    // Pass physical address to controller (CRCR)
    // CRCR bit 0 is RCS (Ring Cycle State)
    write64(op_base + XHCI_CRCR, m_cmd_ring_phys | 1);

    if (g_vga) g_vga->write("xHCI: Data structures allocated.\n");
    return true;
}

bool XHCI::start_controller() {
    uint32_t op_base = m_cap_length;
    
    uint32_t cmd = read32(op_base + XHCI_USBCMD);
    cmd |= USBCMD_RS;
    write32(op_base + XHCI_USBCMD, cmd);
    
    // Wait until HC Halted is cleared
    while (read32(op_base + XHCI_USBSTS) & USBSTS_HCH) {
        // Spin
    }

    if (g_vga) g_vga->write("xHCI: Controller is RUNNING.\n");
    return true;
}

bool XHCI::start() {
    if (!g_pci) return false;
    
    uintptr_t phys_bar = g_pci->get_xhci_bar();
    if (phys_bar == 0) return false; // No xHCI found
    
    m_mmio_base = phys_bar + pmm_hhdm_offset();

    // The bootloader HHDM only maps RAM, not MMIO!
    // We MUST map the xHCI MMIO space explicitly to prevent a Page Fault.
    // We'll map 4 pages (16KB) to be safe for capability + operational + runtime registers.
    for (int i = 0; i < 4; i++) {
        vmm_map(m_mmio_base + (i * 4096), phys_bar + (i * 4096), VMM_MMIO);
    }
    
    // Parse capabilities
    m_cap_length = read32(XHCI_CAPLENGTH) & 0xFF;
    
    uint32_t params1 = read32(XHCI_HCSPARAMS1);
    m_max_slots = params1 & 0xFF;
    m_max_ports = (params1 >> 24) & 0xFF;
    
    if (!reset_controller()) return false;
    if (!initialize_data_structures()) return false;
    if (!start_controller()) return false;

    return true;
}

void XHCI::stop() {
}

const char* XHCI::get_name() const {
    return "xHCI Controller";
}

// Register as level 5 (after basic dev/pci)
REGISTER_MODULE(g_xhci, XHCI, 5_usb);
