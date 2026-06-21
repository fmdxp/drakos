#pragma once

#include <stdint.h>
#include "input/gamepad.hpp"

// Forward declaration
class XHCI;

class DualSenseDriver : public Input::GamepadDriver {
public:
    DualSenseDriver(uint32_t slot_id, uint8_t ep_num, uint16_t max_packet_size, XHCI* xhci);
    
    // Called by xHCI ISR when a new Interrupt IN report arrives
    void on_report_received();
    
    // Submit next read buffer to the endpoint
    void prime_interrupt();
    
    void process_input() override;
    
    // Set the RGB color of the Lightbar
    void set_led(uint8_t r, uint8_t g, uint8_t b);

private:
    uint32_t m_slot_id;
    uint8_t  m_ep_num;
    uint16_t m_max_packet_size;
    XHCI*    m_xhci;
    
    // DMA buffer (physical) for receiving HID reports
    uintptr_t m_report_buf_phys;
    uint8_t*  m_report_buf_virt;
    
    bool parse_report(const uint8_t* report);
};

extern DualSenseDriver* g_dualsense_driver;
