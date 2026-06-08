#include "pci.hpp"
#include "vga.hpp"

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

void PCI::print_device(const PCIDevice& dev) {
    if (!g_vga) return;
    
    g_vga->write("PCI ");
    print_hex(dev.bus, 2); g_vga->write(":");
    print_hex(dev.device, 2); g_vga->write(":");
    print_hex(dev.function, 1); g_vga->write(" | ");
    
    print_hex(dev.vendor_id, 4); g_vga->write(":");
    print_hex(dev.device_id, 4); g_vga->write(" | Class: ");
    print_hex(dev.class_code, 2); g_vga->write(":");
    print_hex(dev.subclass, 2); g_vga->write("\n");
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
    if (g_vga) {
        g_vga->write("Scanning PCI Bus...\n");
    }
    
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
