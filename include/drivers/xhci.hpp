#pragma once

#include <stdint.h>
#include "module.hpp"

// Forward declaration
class PCI;

class XHCI : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

private:
    uintptr_t m_mmio_base = 0;
    
    // Core capabilities
    uint8_t m_cap_length = 0;
    uint32_t m_max_slots = 0;
    uint32_t m_max_ports = 0;
    
    // Core data structures
    uintptr_t m_dcbaa_phys = 0;
    uintptr_t m_cmd_ring_phys = 0;
    
    // Helper to read MMIO
    uint32_t read32(uint32_t offset);
    void write32(uint32_t offset, uint32_t value);
    
    void write64(uint32_t offset, uint64_t value);
    uint64_t read64(uint32_t offset);
    
    bool reset_controller();
    bool initialize_data_structures();
    bool start_controller();
};

extern XHCI* g_xhci;
