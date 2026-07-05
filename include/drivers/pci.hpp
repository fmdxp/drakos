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

    // Configure MSI for a device to route interrupts to the given IDT vector
    bool configure_msi(uint8_t bus, uint8_t device, uint8_t func, uint8_t vector);

    // Getters for xHCI
    uintptr_t get_xhci_bar() const { return m_xhci_bar; }
    PCIDevice get_xhci_device() const { return m_xhci_device; }

    uintptr_t get_ahci_bar() const { return m_ahci_bar; }
    PCIDevice get_ahci_device() const { return m_ahci_device; }


private:
    uintptr_t m_xhci_bar = 0;
    PCIDevice m_xhci_device = {0};

    uintptr_t m_ahci_bar = 0;
    PCIDevice m_ahci_device = {0};

    void check_bus(uint8_t bus);
    void check_device(uint8_t bus, uint8_t device);
    void check_function(uint8_t bus, uint8_t device, uint8_t func);
    
    // Internal method to pretty print the device info
    void print_device(const PCIDevice& dev);
};

extern PCI* g_pci;
