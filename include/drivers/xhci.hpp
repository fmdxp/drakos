#pragma once

#include <stdint.h>
#include "module.hpp"

// Forward declaration
class PCI;

struct TRB {
    uint32_t parameter1;
    uint32_t parameter2;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

// TRB Types
#define TRB_LINK                 6
#define TRB_CMD_ENABLE_SLOT      9
#define TRB_EVENT_CMD_COMPLETION 33
#define TRB_EVENT_PORT_STATUS    34

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
    uint32_t  m_cmd_ring_index = 0;
    bool      m_cmd_ring_pcs = true; // Producer Cycle State
    
    uintptr_t m_erst_phys = 0;
    uintptr_t m_event_ring_phys = 0;
    uint32_t  m_event_ring_index = 0;
    bool      m_event_ring_ccs = true;
    
    // Helper to read MMIO
    uint32_t read32(uint32_t offset);
    void write32(uint32_t offset, uint32_t value);
    
    void write64(uint32_t offset, uint64_t value);
    uint64_t read64(uint32_t offset);
    void ring_doorbell(uint8_t doorbell, uint32_t target);
    
    bool reset_controller();
    bool initialize_data_structures();
    bool configure_interrupter();
    bool start_controller();
    void enumerate_ports();
    void reset_port(uint32_t port_num);
    void send_command(const TRB& trb);

public:
    void handle_interrupt();
};

extern XHCI* g_xhci;

#include "idt.hpp"
extern "C" __attribute__((interrupt)) void xhci_isr(InterruptFrame* frame);
