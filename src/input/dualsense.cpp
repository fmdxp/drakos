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


#include "input/dualsense.hpp"
#include "input/gamepad.hpp"
#include "drivers/xhci.hpp"
#include "pmm.hpp"
#include "vmm.hpp"
#include "vga.hpp"
#include "thread.hpp"

#include "input/hid_transport.hpp"

DualSenseDriver* g_dualsense_driver = nullptr;

DualSenseDriver::DualSenseDriver(HIDTransport* transport)
    : m_transport(transport)
{
    // Register a slot in the GamepadManager
    m_gamepad_index = Input::GamepadManager::register_gamepad(Input::GamepadType::PlayStation5);

    if (g_vga) g_vga->write("DualSense: Driver initialized, waiting for input...\n");

    // Assign the global pointer
    g_dualsense_driver = this;

    // Start listening on the transport
    m_transport->start_listening(this);

    // Send initial LED output report to enable extended mode (touchpad & lightbar)
    set_led(0, 0, 255);
}

void DualSenseDriver::on_report_received(const uint8_t* report, size_t length) {
    (void)length;

    // Debug print the first 10 bytes of the report
    static int debug_prints = 0;
    if (g_vga && debug_prints < 10) {
        g_vga->write("DualSense: Report Bytes: ");
        for (int i = 0; i < 10; i++) {
            char buf[3];
            uint8_t val = report[i];
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

    parse_report(report);

    // Wake gamepad thread
    extern volatile int g_gamepad_pending_reports;
    g_gamepad_pending_reports = g_gamepad_pending_reports + 1;
    extern Thread* g_gamepad_thread;
    if (g_gamepad_thread) {
        extern void scheduler_wake_thread(Thread* t);
        scheduler_wake_thread(g_gamepad_thread);
    }
}

bool DualSenseDriver::parse_report(const uint8_t* r) {
    Input::GamepadState* state = Input::GamepadManager::get_gamepad(m_gamepad_index);
    if (!state) return false;

    Input::GamepadState old_state = *state;
    uint8_t report_id = r[0];

    if (report_id == 0x01) {
        // Simple Report format (Bluetooth default)
        state->left_stick_x  = r[1];
        state->left_stick_y  = r[2];
        state->right_stick_x = r[3];
        state->right_stick_y = r[4];

        // Byte 5: D-Pad (bits 0-3) + Face Buttons (bits 4-7)
        uint8_t dpad = r[5] & 0x0F;
        state->dpad_up    = (dpad == 0 || dpad == 1 || dpad == 7);
        state->dpad_right = (dpad == 1 || dpad == 2 || dpad == 3);
        state->dpad_down  = (dpad == 3 || dpad == 4 || dpad == 5);
        state->dpad_left  = (dpad == 5 || dpad == 6 || dpad == 7);

        state->btn_x = (r[5] >> 4) & 1; // Square
        state->btn_a = (r[5] >> 5) & 1; // Cross
        state->btn_b = (r[5] >> 6) & 1; // Circle
        state->btn_y = (r[5] >> 7) & 1; // Triangle

        // Byte 6: Shoulder / System buttons
        state->btn_l1      = (r[6] >> 0) & 1;
        state->btn_r1      = (r[6] >> 1) & 1;
        state->btn_share   = (r[6] >> 4) & 1;
        state->btn_options = (r[6] >> 5) & 1;
        state->btn_l3      = (r[6] >> 6) & 1;
        state->btn_r3      = (r[6] >> 7) & 1;

        // Byte 7: PS / Touchpad Click
        state->btn_logo     = (r[7] >> 0) & 1;
        state->btn_touchpad = (r[7] >> 1) & 1;

        // Bytes 8 & 9: Analog Triggers
        state->left_trigger  = r[8];
        state->right_trigger = r[9];

    } else if (report_id == 0x31) {
        // Extended Report format (USB / Extended BT)
        state->left_stick_x  = r[1];
        state->left_stick_y  = r[2];
        state->right_stick_x = r[3];
        state->right_stick_y = r[4];

        state->left_trigger  = r[5];
        state->right_trigger = r[6];

        uint8_t dpad = r[8] & 0x0F;
        state->dpad_up    = (dpad == 0 || dpad == 1 || dpad == 7);
        state->dpad_right = (dpad == 1 || dpad == 2 || dpad == 3);
        state->dpad_down  = (dpad == 3 || dpad == 4 || dpad == 5);
        state->dpad_left  = (dpad == 5 || dpad == 6 || dpad == 7);

        state->btn_x = (r[8] >> 4) & 1;
        state->btn_a = (r[8] >> 5) & 1;
        state->btn_b = (r[8] >> 6) & 1;
        state->btn_y = (r[8] >> 7) & 1;

        state->btn_l1      = (r[9] >> 0) & 1;
        state->btn_r1      = (r[9] >> 1) & 1;
        state->btn_share   = (r[9] >> 4) & 1;
        state->btn_options = (r[9] >> 5) & 1;
        state->btn_l3      = (r[9] >> 6) & 1;
        state->btn_r3      = (r[9] >> 7) & 1;

        state->btn_logo     = (r[10] >> 0) & 1;
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
    } else {
        return false;
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


void DualSenseDriver::init_lightbar()
{
    set_led(0, 0, 0);

    // placeholder delay bc our timer sucksssss
    for (int i = 0; i < 1000000; i++) asm volatile("nop");

    set_led(0, 255, 255);
}



void DualSenseDriver::set_led(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t out[48] = {0};
    out[0] = 0xFF; // valid_flag0: enable all valid controls (0xFF)
    out[1] = 0xFF; // valid_flag1: enable all valid controls (0xFF)
    out[2] = 0x1F; // valid_flag2: enable all valid controls (0x1F)

    out[39] = 0x02; // lightbar_setup: 0x02 = setup lightbar
    out[40] = 0x00; // led_brightness: 0 = high
    out[41] = 0x04; // player_leds: 0x04 = Player 1 center LED

    out[42] = r;    // Red
    out[43] = g;    // Green
    out[44] = b;    // Blue
    
    m_transport->send_output_report(0x02, out, sizeof(out));

    if (g_vga) g_vga->write("LED Packet sent\n");
}
