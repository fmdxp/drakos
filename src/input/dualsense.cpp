#include "input/dualsense.hpp"
#include "input/gamepad.hpp"
#include "drivers/xhci.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"

DualSenseDriver* g_dualsense_driver = nullptr;

DualSenseDriver::DualSenseDriver(uint32_t slot_id, uint8_t ep_num, uint16_t max_packet_size, XHCI* xhci)
    : m_slot_id(slot_id), m_ep_num(ep_num), m_max_packet_size(max_packet_size), m_xhci(xhci)
{
    // Register a slot in the GamepadManager
    m_gamepad_index = Input::GamepadManager::register_gamepad(Input::GamepadType::PlayStation5);

    // Allocate DMA buffer for HID reports (physical, contiguous)
    m_report_buf_phys = pmm_alloc(1);
    m_report_buf_virt = reinterpret_cast<uint8_t*>(m_report_buf_phys + pmm_hhdm_offset());

    // Configure the Interrupt IN endpoint on xHCI
    // ep_type = 7 = Interrupt IN (xHCI spec Table 60)
    xhci->configure_endpoint(m_slot_id, m_ep_num, 7, m_max_packet_size, 4);

    if (g_vga) g_vga->write("DualSense: Driver initialized, listening for input...\n");

    // Submit first read
    prime_interrupt();
}

void DualSenseDriver::prime_interrupt() {
    // Submit a Normal TRB on the Interrupt IN endpoint to receive the next HID Report
    m_xhci->submit_interrupt_in(m_slot_id, reinterpret_cast<void*>(m_report_buf_phys), m_max_packet_size);
}

void DualSenseDriver::on_report_received() {
    // Debug print the first 10 bytes of the report
    static int debug_prints = 0;
    if (g_vga && debug_prints < 10) {
        g_vga->write("DualSense: Report Bytes: ");
        for (int i = 0; i < 10; i++) {
            char buf[3];
            uint8_t val = m_report_buf_virt[i];
            const char* hex = "0123456789ABCDEF";
            buf[0] = hex[val >> 4];
            buf[1] = hex[val & 0x0F];
            buf[2] = '\0';
            g_vga->write(buf);
            g_vga->write(" ");
        }
        g_vga->write("\n");
        debug_prints++;
    }

    // Called by xHCI ISR when TRB_EVENT_TRANSFER fires for this slot
    parse_report(m_report_buf_virt);

    // Re-submit buffer to keep the stream going
    prime_interrupt();
}

void DualSenseDriver::parse_report(const uint8_t* r) {
    // DualSense USB HID Report format (Report ID 0x01):
    // Byte 0:   Report ID (0x01)
    // Byte 1:   Left Stick X
    // Byte 2:   Left Stick Y
    // Byte 3:   Right Stick X
    // Byte 4:   Right Stick Y
    // Byte 5:   L2 Trigger (Analog)
    // Byte 6:   R2 Trigger (Analog)
    // Byte 7:   Sequence Number
    // Byte 8:   D-pad (nibble) + face buttons (nibble)
    // Byte 9:   L1/R1, Share, Options, L3, R3
    // Byte 10:  PS Button, Touchpad Click

    if (r[0] != 0x01) return; // Ignore non-standard reports

    Input::GamepadState* state = Input::GamepadManager::get_gamepad(m_gamepad_index);
    if (!state) return;

    // Analog sticks
    state->left_stick_x  = r[1];
    state->left_stick_y  = r[2];
    state->right_stick_x = r[3];
    state->right_stick_y = r[4];

    // Triggers (analog)
    state->left_trigger  = r[5];
    state->right_trigger = r[6];

    // D-Pad (lower nibble of byte 8)
    uint8_t dpad = r[8] & 0x0F;
    state->dpad_up    = (dpad == 0 || dpad == 1 || dpad == 7);
    state->dpad_right = (dpad == 1 || dpad == 2 || dpad == 3);
    state->dpad_down  = (dpad == 3 || dpad == 4 || dpad == 5);
    state->dpad_left  = (dpad == 5 || dpad == 6 || dpad == 7);

    // Face buttons (upper nibble of byte 8)
    state->btn_x = (r[8] >> 4) & 1; // Square
    state->btn_a = (r[8] >> 5) & 1; // Cross
    state->btn_b = (r[8] >> 6) & 1; // Circle
    state->btn_y = (r[8] >> 7) & 1; // Triangle

    // Shoulder / System buttons (byte 9)
    state->btn_l1      = (r[9] >> 0) & 1;
    state->btn_r1      = (r[9] >> 1) & 1;
    state->btn_share   = (r[9] >> 4) & 1; // Create
    state->btn_options = (r[9] >> 5) & 1;
    state->btn_l3      = (r[9] >> 6) & 1;
    state->btn_r3      = (r[9] >> 7) & 1;

    // PS / Logo button (byte 10)
    state->btn_logo = (r[10] >> 0) & 1;
}

void DualSenseDriver::process_input() {
    // In our architecture, the ISR calls on_report_received() directly.
    // This method is available for polling-mode usage if needed.
    on_report_received();
}
