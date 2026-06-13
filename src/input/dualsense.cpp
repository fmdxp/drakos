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
    // Byte 5:   D-pad (nibble) + face buttons (nibble) -> lower nibble = dpad, upper nibble = square/cross/circle/triangle
    // Byte 6:   R3 | L3 | Options | Create | R1 | L1 | (lower bits)
    // Byte 7:   Touch | PS | Mute | (lower bits)
    // Byte 8:   L2 analog
    // Byte 9:   R2 analog

    if (r[0] != 0x01) return; // Ignore non-standard reports

    Input::GamepadState* state = Input::GamepadManager::get_gamepad(m_gamepad_index);
    if (!state) return;

    // Analog sticks
    state->left_stick_x  = r[1];
    state->left_stick_y  = r[2];
    state->right_stick_x = r[3];
    state->right_stick_y = r[4];

    // Triggers (analog)
    state->left_trigger  = r[8];
    state->right_trigger = r[9];

    // D-Pad (lower nibble of byte 5)
    uint8_t dpad = r[5] & 0x0F;
    state->dpad_up    = (dpad == 0 || dpad == 1 || dpad == 7);
    state->dpad_right = (dpad == 1 || dpad == 2 || dpad == 3);
    state->dpad_down  = (dpad == 3 || dpad == 4 || dpad == 5);
    state->dpad_left  = (dpad == 5 || dpad == 6 || dpad == 7);

    // Face buttons (upper nibble of byte 5)
    state->btn_x = (r[5] >> 4) & 1; // Square
    state->btn_a = (r[5] >> 5) & 1; // Cross
    state->btn_b = (r[5] >> 6) & 1; // Circle
    state->btn_y = (r[5] >> 7) & 1; // Triangle

    // Shoulder / System buttons (byte 6)
    state->btn_l1      = (r[6] >> 0) & 1;
    state->btn_r1      = (r[6] >> 1) & 1;
    state->btn_share   = (r[6] >> 4) & 1; // Create
    state->btn_options = (r[6] >> 5) & 1;
    state->btn_l3      = (r[6] >> 6) & 1;
    state->btn_r3      = (r[6] >> 7) & 1;

    // PS / Logo button (byte 7)
    state->btn_logo = (r[7] >> 0) & 1;
}

void DualSenseDriver::process_input() {
    // In our architecture, the ISR calls on_report_received() directly.
    // This method is available for polling-mode usage if needed.
    on_report_received();
}
