#include "pci.hpp"
#include "vga.hpp"
#include "vmm.hpp"
#include "pmm.hpp"

// Convert an integer to a hex string manually, since we don't have printf
static void print_hex(uint32_t val, int digits) {
    if (!g_vga) return;
    
    g_vga->write("0x");
    for (int i = digits - 1; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0x0F;
        if (nibble < 10) g_vga->write_char('0' + nibble);
        else g_vga->write_char('A' + (nibble - 10));
    }
}

uint32_t PCI::read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t address = 
        (1 << 31) | 
        (bus << 16) | 
        ((device & 0x1F) << 11) | 
        ((func & 0x07) << 8) | 
        (offset & 0xFC);
        
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void PCI::write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = 
        (1 << 31) | 
        (bus << 16) | 
        ((device & 0x1F) << 11) | 
        ((func & 0x07) << 8) | 
        (offset & 0xFC);
        
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

bool PCI::configure_msi(uint8_t bus, uint8_t device, uint8_t func, uint8_t vector) {
    // Check if capabilities list is supported (Status Register Bit 4)
    uint32_t status_cmd = read(bus, device, func, 0x04);
    if ((status_cmd & (1 << 20)) == 0) return false; // 0x04 reads Command (low 16) and Status (high 16). Bit 4 of Status is Bit 20.

    uint8_t cap_ptr = read(bus, device, func, 0x34) & 0xFF;
    
    // Prevent infinite loops in corrupted config spaces
    int max_caps = 48;
    
    while (cap_ptr != 0 && max_caps-- > 0) {
        // cap_ptr must be dword aligned
        cap_ptr &= ~3;
        
        uint32_t cap_header = read(bus, device, func, cap_ptr);
        uint8_t cap_id = cap_header & 0xFF;
        
        // // Debug print to see what capabilities QEMU xHCI exposes
        // if (g_vga) {
        //     g_vga->write("    PCI Cap found: 0x");
        //     char buf[3];
        //     buf[0] = (cap_id >> 4) < 10 ? '0' + (cap_id >> 4) : 'A' + (cap_id >> 4) - 10;
        //     buf[1] = (cap_id & 0x0F) < 10 ? '0' + (cap_id & 0x0F) : 'A' + (cap_id & 0x0F) - 10;
        //     buf[2] = '\0';
        //     g_vga->write(buf);
        //     g_vga->write("\n");
        // }
        
        if (cap_id == 0x05) { // MSI Capability
            uint16_t msg_ctrl = (cap_header >> 16) & 0xFFFF;
            bool is_64bit = (msg_ctrl & (1 << 7)) != 0;
            
            // MSI Address: 0xFEE00000 | (APIC_ID << 12). 
            // Assuming APIC ID 0 (BSP) for now.
            uint32_t msi_addr = 0xFEE00000;
            write(bus, device, func, cap_ptr + 4, msi_addr);
            
            if (is_64bit) {
                write(bus, device, func, cap_ptr + 8, 0); // Upper 32 bits of address
                write(bus, device, func, cap_ptr + 12, vector); // Data is the vector
            } else {
                write(bus, device, func, cap_ptr + 8, vector); // Data is the vector
            }
            
            // Enable MSI (Bit 0 of Message Control)
            msg_ctrl |= 1;
            uint32_t new_header = (cap_header & 0x0000FFFF) | ((uint32_t)msg_ctrl << 16);
            write(bus, device, func, cap_ptr, new_header);
            
            if (g_vga) g_vga->write("    -> MSI Configured!\n");
            return true;
        } else if (cap_id == 0x11) { // MSI-X Capability
            uint16_t msg_ctrl = (cap_header >> 16) & 0xFFFF;
            
            uint32_t table_info = read(bus, device, func, cap_ptr + 4);
            uint8_t bir = table_info & 0x07;
            uint32_t table_offset = table_info & ~0x07;
            
            // Get BAR address
            uint32_t bar_offset = 0x10 + (bir * 4);
            uint32_t bar_low = read(bus, device, func, bar_offset);
            uintptr_t table_phys = 0;
            
            if ((bar_low & 0x06) == 0x04) { // 64-bit BAR
                uint32_t bar_high = read(bus, device, func, bar_offset + 4);
                table_phys = (bar_low & 0xFFFFFFF0) | ((uintptr_t)bar_high << 32);
            } else {
                table_phys = (bar_low & 0xFFFFFFF0);
            }
            
            table_phys += table_offset;
            
            // Ensure the MSI-X table is mapped in the HHDM
            uintptr_t table_virt = table_phys + pmm_hhdm_offset();
            vmm_map(table_virt & ~0xFFFULL, table_phys & ~0xFFFULL, VMM_MMIO);
            
            // Write Entry 0
            volatile uint32_t* msix_table = reinterpret_cast<volatile uint32_t*>(table_virt);
            msix_table[0] = 0xFEE00000; // Msg Addr Low (APIC)
            msix_table[1] = 0;          // Msg Addr High
            msix_table[2] = vector;     // Msg Data (Vector)
            msix_table[3] = 0;          // Vector Control (0 = Unmasked)
            
            // Enable MSI-X (Bit 15) and clear Function Mask (Bit 14)
            msg_ctrl |= (1 << 15);
            msg_ctrl &= ~(1 << 14);
            
            uint32_t new_header = (cap_header & 0x0000FFFF) | ((uint32_t)msg_ctrl << 16);
            write(bus, device, func, cap_ptr, new_header);
            
            if (g_vga) g_vga->write("    -> MSI-X Configured!\n");
            return true;
        }
        
        cap_ptr = (cap_header >> 8) & 0xFF;
    }
    
    return false;
}

void PCI::print_device(const PCIDevice& dev) {
    if (!g_vga) return;
    
    // g_vga->write("PCI ");
    // print_hex(dev.bus, 2); g_vga->write(":");
    // print_hex(dev.device, 2); g_vga->write(":");
    // print_hex(dev.function, 1); g_vga->write(" | ");
    
    // print_hex(dev.vendor_id, 4); g_vga->write(":");
    // print_hex(dev.device_id, 4); g_vga->write(" | Class: ");
    // print_hex(dev.class_code, 2); g_vga->write(":");
    // print_hex(dev.subclass, 2); g_vga->write("\n");
}

void PCI::check_function(uint8_t bus, uint8_t device, uint8_t func) {
    uint32_t reg0 = read(bus, device, func, 0x00);
    uint16_t vendor_id = reg0 & 0xFFFF;
    
    if (vendor_id == 0xFFFF) return; // Device doesn't exist
    
    uint16_t device_id = (reg0 >> 16) & 0xFFFF;
    
    uint32_t reg8 = read(bus, device, func, 0x08);
    uint8_t class_code = (reg8 >> 24) & 0xFF;
    uint8_t subclass = (reg8 >> 16) & 0xFF;
    uint8_t prog_if = (reg8 >> 8) & 0xFF;
    
    PCIDevice pci_dev = {bus, device, func, vendor_id, device_id, class_code, subclass, prog_if};
    
    print_device(pci_dev);

    // Is this an xHCI Controller?
    if (class_code == 0x0C && subclass == 0x03 && prog_if == 0x30) {
        // Read BAR0 (Offset 0x10) and BAR1 (Offset 0x14) for 64-bit address
        uint32_t bar0 = read(bus, device, func, 0x10);
        uint32_t bar1 = read(bus, device, func, 0x14);
        
        uintptr_t mmio_base = 0;
        
        // Check if it's a 64-bit BAR
        if ((bar0 & 0x06) == 0x04) {
            mmio_base = (bar0 & 0xFFFFFFF0) | ((uintptr_t)bar1 << 32);
        } else {
            mmio_base = (bar0 & 0xFFFFFFF0);
        }
        
        m_xhci_bar = mmio_base;
        m_xhci_device = pci_dev;
        
        // Enable Bus Mastering (Bit 2) and Memory Space (Bit 1) in Command Register (Offset 0x04)
        uint32_t cmd = read(bus, device, func, 0x04);
        cmd |= (1 << 2) | (1 << 1);
        write(bus, device, func, 0x04, cmd);
        
        // if (g_vga) {
        //     g_vga->write("  -> xHCI Controller Found and Enabled!\n");
        // }
    }
}

void PCI::check_device(uint8_t bus, uint8_t device) {
    uint32_t reg0 = read(bus, device, 0, 0x00);
    if ((reg0 & 0xFFFF) == 0xFFFF) return; // Device doesn't exist
    
    check_function(bus, device, 0);
    
    uint32_t regC = read(bus, device, 0, 0x0C);
    uint8_t header_type = (regC >> 16) & 0xFF;
    
    // Check if it's a multi-function device
    if ((header_type & 0x80) != 0) {
        for (uint8_t func = 1; func < 8; func++) {
            check_function(bus, device, func);
        }
    }
}

void PCI::check_bus(uint8_t bus) {
    for (uint8_t device = 0; device < 32; device++) {
        check_device(bus, device);
    }
}

bool PCI::start() {
    // if (g_vga) {
    //     g_vga->write("Scanning PCI Bus...\n");
    // }
    
    // In a full implementation we would check the MCFG table via ACPI.
    // For now, standard Port I/O works well for buses 0-255.
    
    // Bus 0 is the root bus
    uint32_t regC = read(0, 0, 0, 0x0C);
    uint8_t header_type = (regC >> 16) & 0xFF;
    
    if ((header_type & 0x80) == 0) {
        // Single PCI host controller
        check_bus(0);
    } else {
        // Multiple PCI host controllers
        for (uint8_t func = 0; func < 8; func++) {
            if ((read(0, 0, func, 0x00) & 0xFFFF) != 0xFFFF) {
                check_bus(func);
            }
        }
    }

    return true;
}

void PCI::stop() {
}

const char* PCI::get_name() const {
    return "PCI Subsystem";
}

// We register this at level 4, alongside other device drivers
REGISTER_MODULE(g_pci, PCI, 4_dev);
