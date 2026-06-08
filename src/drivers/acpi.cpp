#include "acpi.hpp"
#include "limine_requests.hpp"
#include "pmm.hpp" // For HHDM offset

// Helper to string match signatures
static bool check_signature(const char* sig, const char* target) {
    for (int i = 0; i < 4; i++) {
        if (sig[i] != target[i]) return false;
    }
    return true;
}

bool ACPI::start() {
    if (!g_rsdp_request.response) {
        return false; // No ACPI provided by bootloader
    }

    RSDPDescriptor20* rsdp = reinterpret_cast<RSDPDescriptor20*>(g_rsdp_request.response->address);
    
    // We prefer XSDT (64-bit) if revision >= 2, otherwise RSDT (32-bit)
    uint8_t revision = rsdp->firstPart.Revision;
    
    ACPISDTHeader* root_table = nullptr;
    size_t entries = 0;
    bool is_xsdt = false;

    if (revision >= 2 && rsdp->XsdtAddress != 0) {
        root_table = reinterpret_cast<ACPISDTHeader*>(rsdp->XsdtAddress + pmm_hhdm_offset());
        entries = (root_table->Length - sizeof(ACPISDTHeader)) / 8;
        is_xsdt = true;
    } else {
        root_table = reinterpret_cast<ACPISDTHeader*>(rsdp->firstPart.RsdtAddress + pmm_hhdm_offset());
        entries = (root_table->Length - sizeof(ACPISDTHeader)) / 4;
        is_xsdt = false;
    }

    // Iterate over all tables looking for the MADT ("APIC")
    uint8_t* table_pointers = reinterpret_cast<uint8_t*>(root_table) + sizeof(ACPISDTHeader);
    
    for (size_t i = 0; i < entries; i++) {
        uintptr_t table_addr = 0;
        
        if (is_xsdt) {
            table_addr = *reinterpret_cast<uint64_t*>(table_pointers + i * 8);
        } else {
            table_addr = *reinterpret_cast<uint32_t*>(table_pointers + i * 4);
        }

        ACPISDTHeader* header = reinterpret_cast<ACPISDTHeader*>(table_addr + pmm_hhdm_offset());

        if (check_signature(header->Signature, "APIC")) {
            parse_madt(reinterpret_cast<MADT*>(header));
            break; // Found what we need
        }
    }

    return (m_lapic_phys != 0);
}

void ACPI::parse_madt(MADT* madt) {
    // The base Local APIC address from the header
    m_lapic_phys = madt->localApicAddress;

    // Parse the variable-length entries
    uint8_t* ptr = madt->entries;
    uint8_t* end = reinterpret_cast<uint8_t*>(madt) + madt->header.Length;

    while (ptr < end) {
        uint8_t type = ptr[0];
        uint8_t length = ptr[1];

        // Type 1: I/O APIC
        if (type == 1) {
            uint32_t ioapic_addr = *reinterpret_cast<uint32_t*>(ptr + 4);
            // In a real OS we might have multiple I/O APICs. We just store the first one here.
            if (m_ioapic_phys == 0) {
                m_ioapic_phys = ioapic_addr;
            }
        }
        
        // Type 5: Local APIC Address Override (for 64-bit systems)
        else if (type == 5) {
            m_lapic_phys = *reinterpret_cast<uint64_t*>(ptr + 4);
        }

        ptr += length;
    }
}

void ACPI::stop() {
}

const char* ACPI::get_name() const {
    return "ACPI Subsystem";
}

// We register this as a driver level component because it relies on PMM mapping
REGISTER_MODULE(g_acpi, ACPI, 3_drv);
