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
