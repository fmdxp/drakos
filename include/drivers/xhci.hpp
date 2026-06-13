#pragma once

#include <stdint.h>
#include "module.hpp"
#include "usb/usb.hpp"

// Forward declaration
class PCI;

struct TRB {
    uint32_t parameter1;
    uint32_t parameter2;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

// TRB Types
#define TRB_NORMAL               1
#define TRB_SETUP_STAGE          2
#define TRB_DATA_STAGE           3
#define TRB_STATUS_STAGE         4
#define TRB_LINK                 6
#define TRB_CMD_ENABLE_SLOT      9
#define TRB_CMD_ADDRESS_DEVICE   11
#define TRB_EVENT_TRANSFER       32
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
    
    uint32_t m_current_port_num = 0;
    uint32_t m_current_port_speed = 0;
    
    // EP0 Transfer Rings per slot
    uintptr_t m_ep0_rings_phys[256] = {0};
    uint32_t  m_ep0_ring_indices[256] = {0};
    bool      m_ep0_ring_pcs[256] = {false};
    
    // Interrupt Transfer Rings (Simplified: 1 per slot, hardcoded to whatever EP is configured)
    uintptr_t m_int_rings_phys[256] = {0};
    uint32_t  m_int_ring_indices[256] = {0};
    bool      m_int_ring_pcs[256] = {false};
    uint8_t   m_int_ep_dci[256] = {0}; // The DCI index used for the interrupt ring
    
    volatile bool m_transfer_complete = false;
    volatile bool m_port_setup_complete = false;
    
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
    void configure_slot(uint32_t slot_id, uint32_t port_speed, uint32_t port_num);

public:
    bool do_control_transfer(uint32_t slot_id, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint16_t length, void* buffer_phys);
    bool configure_endpoint(uint32_t slot_id, uint8_t ep_num, uint8_t ep_type, uint16_t max_packet_size, uint8_t interval);
    bool submit_interrupt_in(uint32_t slot_id, void* buffer_phys, uint32_t length);
    
    void handle_interrupt();
    void poll_event_ring();
};

extern XHCI* g_xhci;

#include "idt.hpp"
extern "C" __attribute__((interrupt)) void xhci_isr(InterruptFrame* frame);
