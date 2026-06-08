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

// ---------------------------------------------------------
// ACPI Table Structures
// ---------------------------------------------------------

struct RSDPDescriptor {
    char Signature[8];
    uint8_t Checksum;
    char OEMID[6];
    uint8_t Revision;
    uint32_t RsdtAddress;
} __attribute__ ((packed));

struct RSDPDescriptor20 {
    RSDPDescriptor firstPart;
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t ExtendedChecksum;
    uint8_t reserved[3];
} __attribute__ ((packed));

struct ACPISDTHeader {
    char Signature[4];
    uint32_t Length;
    uint8_t Revision;
    uint8_t Checksum;
    char OEMID[6];
    char OEMTableID[8];
    uint32_t OEMRevision;
    uint32_t CreatorID;
    uint32_t CreatorRevision;
} __attribute__ ((packed));

struct MADT {
    ACPISDTHeader header;
    uint32_t localApicAddress;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

// ---------------------------------------------------------
// ACPI Subsystem Module
// ---------------------------------------------------------

class ACPI : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

    // Public getters for other hardware subsystems
    uintptr_t get_lapic_phys() const { return m_lapic_phys; }
    uintptr_t get_ioapic_phys() const { return m_ioapic_phys; }

private:
    uintptr_t m_lapic_phys = 0;
    uintptr_t m_ioapic_phys = 0;

    void parse_madt(MADT* madt);
};

extern ACPI* g_acpi;
