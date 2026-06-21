#include "input/dualsense.hpp"
#include "input/gamepad.hpp"
#include "drivers/xhci.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"
#include "thread.hpp"

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

    // Assign the global pointer EARLY! 
    // Otherwise, the xHCI interrupt for the first read might fire while we are 
    // blocked in set_led(), and it will see a null pointer and drop the stream!
    g_dualsense_driver = this;

    // Submit first read
    prime_interrupt();
    
    // Light up the LED to Cyan to show it's working!
    set_led(0, 255, 255);
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
    bool changed = parse_report(m_report_buf_virt);

    // SVEGLIA IL KERNEL THREAD (SOLO SE C'E' UN CAMBIAMENTO REALE!)
    // I controller moderni spammano pacchetti a 500Hz per via del "rumore" analogico
    // delle levette. Filtriamo questi micro-movimenti per mantenere la CPU in idle!
    if (changed) {
        extern Thread* g_kernel_thread;
        if (g_kernel_thread) {
            extern void scheduler_wake_thread(Thread* t);
            scheduler_wake_thread(g_kernel_thread);
        }
    }

    // Re-submit buffer to keep the stream going
    prime_interrupt();
}

bool DualSenseDriver::parse_report(const uint8_t* r) {
    if (r[0] != 0x01) return false;

    Input::GamepadState* state = Input::GamepadManager::get_gamepad(m_gamepad_index);
    if (!state) return false;

    Input::GamepadState old_state = *state;

    // Analog sticks
    state->left_stick_x  = r[1];
    state->left_stick_y  = r[2];
    state->right_stick_x = r[3];
    state->right_stick_y = r[4];

    // Triggers
    state->left_trigger  = r[5];
    state->right_trigger = r[6];

    // D-Pad
    uint8_t dpad = r[8] & 0x0F;
    state->dpad_up    = (dpad == 0 || dpad == 1 || dpad == 7);
    state->dpad_right = (dpad == 1 || dpad == 2 || dpad == 3);
    state->dpad_down  = (dpad == 3 || dpad == 4 || dpad == 5);
    state->dpad_left  = (dpad == 5 || dpad == 6 || dpad == 7);

    // Face buttons
    state->btn_x = (r[8] >> 4) & 1;
    state->btn_a = (r[8] >> 5) & 1;
    state->btn_b = (r[8] >> 6) & 1;
    state->btn_y = (r[8] >> 7) & 1;

    // Shoulder / System
    state->btn_l1      = (r[9] >> 0) & 1;
    state->btn_r1      = (r[9] >> 1) & 1;
    state->btn_share   = (r[9] >> 4) & 1;
    state->btn_options = (r[9] >> 5) & 1;
    state->btn_l3      = (r[9] >> 6) & 1;
    state->btn_r3      = (r[9] >> 7) & 1;

    // PS / Logo / Touchpad Click
    state->btn_logo = (r[10] >> 0) & 1;
    state->btn_touchpad = (r[10] >> 1) & 1;
    
    uint32_t tp1 = 33;
    state->touchpad_touching_1 = ((r[tp1] & 0x80) == 0);
    if (state->touchpad_touching_1) {
        state->touchpad_x_1 = r[tp1 + 1] | ((r[tp1 + 2] & 0x0F) << 8);
        state->touchpad_y_1 = ((r[tp1 + 2] & 0xF0) >> 4) | (r[tp1 + 3] << 4);
    }
    
    uint32_t tp2 = 37;
    state->touchpad_touching_2 = ((r[tp2] & 0x80) == 0);
    if (state->touchpad_touching_2) {
        state->touchpad_x_2 = r[tp2 + 1] | ((r[tp2 + 2] & 0x0F) << 8);
        state->touchpad_y_2 = ((r[tp2 + 2] & 0xF0) >> 4) | (r[tp2 + 3] << 4);
    }

    bool changed = false;
    
    // Check digital buttons
    if (state->btn_a != old_state.btn_a || state->btn_b != old_state.btn_b || 
        state->btn_x != old_state.btn_x || state->btn_y != old_state.btn_y ||
        state->dpad_up != old_state.dpad_up || state->dpad_down != old_state.dpad_down ||
        state->dpad_left != old_state.dpad_left || state->dpad_right != old_state.dpad_right ||
        state->btn_l1 != old_state.btn_l1 || state->btn_r1 != old_state.btn_r1 ||
        state->btn_l3 != old_state.btn_l3 || state->btn_r3 != old_state.btn_r3 ||
        state->btn_options != old_state.btn_options || state->btn_share != old_state.btn_share ||
        state->btn_logo != old_state.btn_logo || state->btn_touchpad != old_state.btn_touchpad ||
        state->touchpad_touching_1 != old_state.touchpad_touching_1 ||
        state->touchpad_touching_2 != old_state.touchpad_touching_2) {
        changed = true;
    }
    
    // Check analog values (allow a jitter deadzone of +/- 2)
    auto diff = [](uint8_t a, uint8_t b) { return a > b ? a - b : b - a; };
    if (diff(state->left_trigger, old_state.left_trigger) > 2 ||
        diff(state->right_trigger, old_state.right_trigger) > 2 ||
        diff(state->left_stick_x, old_state.left_stick_x) > 2 ||
        diff(state->left_stick_y, old_state.left_stick_y) > 2 ||
        diff(state->right_stick_x, old_state.right_stick_x) > 2 ||
        diff(state->right_stick_y, old_state.right_stick_y) > 2) {
        changed = true;
    }

    return changed;
}

void DualSenseDriver::process_input() {
    // Process input (optional abstraction logic here)
}

void DualSenseDriver::set_led(uint8_t r, uint8_t g, uint8_t b) {
    // DualSense USB Output Report (Report ID 0x02)
    // Common struct is 47 bytes, starting at offset 1
    // valid_flag1 is at offset 2. Lightbar flag is 0x04.
    // valid_flag2 is at offset 39. Lightbar Setup is 0x02.
    // lightbar_setup is at offset 42 (1 = slow fade, 2 = solid).
    // lightbar_red = 45, green = 46, blue = 47.
    
    uintptr_t out_buf_phys = pmm_alloc(1);
    uint8_t* out = reinterpret_cast<uint8_t*>(out_buf_phys + pmm_hhdm_offset());
    for (int i = 0; i < 64; i++) out[i] = 0;
    
    out[0] = 0x02; // Report ID
    out[1] = 0x00; // valid_flag0
    out[2] = 0x04; // valid_flag1: DS_OUTPUT_VALID_FLAG1_LIGHTBAR_CONTROL_ENABLE
    out[39] = 0x02; // valid_flag2: DS_OUTPUT_VALID_FLAG2_LIGHTBAR_SETUP_CONTROL_ENABLE
    out[42] = 0x01; // lightbar_setup: 1 = ON / fade in
    out[43] = 0x00; // led_brightness: high
    out[44] = 0x00; // player_leds
    out[45] = r;    // Red
    out[46] = g;    // Green
    out[47] = b;    // Blue
    
    // Find the HID interface number to send to (usually 3)
    uint8_t hid_intf = 3;
    
    // Send SET_REPORT (Output). Type=0x21, Request=0x09, Value=0x0202
    m_xhci->do_control_transfer(m_slot_id, 0x21, 0x09, 0x0202, hid_intf, 63, reinterpret_cast<void*>(out_buf_phys));
}
