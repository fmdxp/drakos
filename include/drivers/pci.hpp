#pragma once

#include <stdint.h>
#include "module.hpp"

// Utility functions for x86 in/out/outl/inl instructions
static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// PCI Access Ports
#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

struct PCIDevice {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
};

class PCI : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

    // Read a 32-bit register from PCI Configuration Space
    uint32_t read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
    
    // Write a 32-bit register to PCI Configuration Space
    void write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);

    // Getters for xHCI
    uintptr_t get_xhci_bar() const { return m_xhci_bar; }
    PCIDevice get_xhci_device() const { return m_xhci_device; }

private:
    uintptr_t m_xhci_bar = 0;
    PCIDevice m_xhci_device = {0};
    void check_bus(uint8_t bus);
    void check_device(uint8_t bus, uint8_t device);
    void check_function(uint8_t bus, uint8_t device, uint8_t func);
    
    // Internal method to pretty print the device info
    void print_device(const PCIDevice& dev);
};

extern PCI* g_pci;
