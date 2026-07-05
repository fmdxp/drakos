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


#include "gamepad.hpp"
#include "vga.hpp"
#include "thread.hpp"

namespace Input {

GamepadState GamepadManager::gamepads[4] {};

int GamepadManager::register_gamepad(GamepadType type) {
    for (int i = 0; i < 4; i++) {
        if (!gamepads[i].connected) {
            gamepads[i].connected = true;
            gamepads[i].type = type;
            
            if (g_vga) {
                g_vga->write("GamepadManager: Registered new Gamepad on Slot ");
                char buf[2];
                buf[0] = i + '0';
                buf[1] = '\0';
                g_vga->write(buf);
                g_vga->write("\n");
            }
            return i;
        }
    }
    
    if (g_vga) g_vga->write("GamepadManager: Failed to register Gamepad, all slots full!\n");
    return -1;
}

void GamepadManager::gamepad_debug_poll() {
    // Process any USB devices that were registered during an interrupt
    
    // Print gamepad state changes
    static Input::GamepadState last_gp_state {};
    Input::GamepadState* gp = Input::GamepadManager::get_gamepad(0);
    if (gp && gp->connected) {
        bool changed = false;
        if (gp->btn_a != last_gp_state.btn_a) changed = true;
        if (gp->btn_b != last_gp_state.btn_b) changed = true;
        if (gp->btn_x != last_gp_state.btn_x) changed = true;
        if (gp->btn_y != last_gp_state.btn_y) changed = true;
        if (gp->dpad_up != last_gp_state.dpad_up) changed = true;
        if (gp->dpad_down != last_gp_state.dpad_down) changed = true;
        if (gp->dpad_left != last_gp_state.dpad_left) changed = true;
        if (gp->dpad_right != last_gp_state.dpad_right) changed = true;
        if (gp->btn_l1 != last_gp_state.btn_l1) changed = true;
        if (gp->btn_r1 != last_gp_state.btn_r1) changed = true;
        if (gp->btn_l3 != last_gp_state.btn_l3) changed = true;
        if (gp->btn_r3 != last_gp_state.btn_r3) changed = true;
        if (gp->btn_share != last_gp_state.btn_share) changed = true;
        if (gp->btn_options != last_gp_state.btn_options) changed = true;
        if (gp->btn_logo != last_gp_state.btn_logo) changed = true;
        
        // Analog sticks have jitter, so we use a deadzone of 2 to avoid infinite spam
        auto diff = [](uint8_t a, uint8_t b) { return a > b ? a - b : b - a; };
        if (diff(gp->left_stick_x, last_gp_state.left_stick_x) > 2) changed = true;
        if (diff(gp->left_stick_y, last_gp_state.left_stick_y) > 2) changed = true;
        if (diff(gp->right_stick_x, last_gp_state.right_stick_x) > 2) changed = true;
        if (diff(gp->right_stick_y, last_gp_state.right_stick_y) > 2) changed = true;
        if (diff(gp->left_trigger, last_gp_state.left_trigger) > 2) changed = true;
        if (diff(gp->right_trigger, last_gp_state.right_trigger) > 2) changed = true;
        
        if (gp->touchpad_touching_1 != last_gp_state.touchpad_touching_1) changed = true;
        if (gp->touchpad_touching_1 && diff(gp->touchpad_x_1, last_gp_state.touchpad_x_1) > 2) changed = true;
        if (gp->touchpad_touching_1 && diff(gp->touchpad_y_1, last_gp_state.touchpad_y_1) > 2) changed = true;
        
        static uint32_t print_counter = 0;
        print_counter++;
        // Print every ~500 iterations depending on hlt
        if (print_counter > 500) {
            print_counter = 0;
            changed = true; // Force print periodically
        }

        if (changed) {
            g_vga->write("Gamepad 0: ");
            if (gp->btn_a) g_vga->write("X ");
            if (gp->btn_b) g_vga->write("O ");
            if (gp->btn_x) g_vga->write("SQ ");
            if (gp->btn_y) g_vga->write("TR ");
            if (gp->dpad_up) g_vga->write("UP ");
            if (gp->dpad_down) g_vga->write("DWN ");
            if (gp->dpad_left) g_vga->write("LFT ");
            if (gp->dpad_right) g_vga->write("RGT ");
            
            if (gp->btn_l1) g_vga->write("L1 ");
            if (gp->btn_r1) g_vga->write("R1 ");
            if (gp->btn_l3) g_vga->write("L3 ");
            if (gp->btn_r3) g_vga->write("R3 ");
            if (gp->btn_share) g_vga->write("SHR ");
            if (gp->btn_options) g_vga->write("OPT ");
            if (gp->btn_logo) g_vga->write("PS ");
            
            // Print analog values
            char a_buf[128];
            const char* hex = "0123456789ABCDEF";
            int i = 0;
            
            a_buf[i++] = ' '; a_buf[i++] = 'L'; a_buf[i++] = 'X'; a_buf[i++] = ':';
            a_buf[i++] = hex[(gp->left_stick_x >> 4) & 0xF];
            a_buf[i++] = hex[gp->left_stick_x & 0xF];
            a_buf[i++] = ' '; a_buf[i++] = 'L'; a_buf[i++] = 'Y'; a_buf[i++] = ':';
            a_buf[i++] = hex[(gp->left_stick_y >> 4) & 0xF];
            a_buf[i++] = hex[gp->left_stick_y & 0xF];
            
            a_buf[i++] = ' '; a_buf[i++] = 'R'; a_buf[i++] = 'X'; a_buf[i++] = ':';
            a_buf[i++] = hex[(gp->right_stick_x >> 4) & 0xF];
            a_buf[i++] = hex[gp->right_stick_x & 0xF];
            a_buf[i++] = ' '; a_buf[i++] = 'R'; a_buf[i++] = 'Y'; a_buf[i++] = ':';
            a_buf[i++] = hex[(gp->right_stick_y >> 4) & 0xF];
            a_buf[i++] = hex[gp->right_stick_y & 0xF];
            
            a_buf[i++] = ' '; a_buf[i++] = 'L'; a_buf[i++] = '2'; a_buf[i++] = ':';
            a_buf[i++] = hex[(gp->left_trigger >> 4) & 0xF];
            a_buf[i++] = hex[gp->left_trigger & 0xF];
            a_buf[i++] = ' '; a_buf[i++] = 'R'; a_buf[i++] = '2'; a_buf[i++] = ':';
            a_buf[i++] = hex[(gp->right_trigger >> 4) & 0xF];
            a_buf[i++] = hex[gp->right_trigger & 0xF];
            
            // Trackpad
            a_buf[i++] = ' '; a_buf[i++] = 'T'; a_buf[i++] = 'P'; a_buf[i++] = ':';
            a_buf[i++] = gp->touchpad_touching_1 ? '1' : '0';
            a_buf[i++] = ' '; a_buf[i++] = 'T'; a_buf[i++] = 'X'; a_buf[i++] = ':';
            a_buf[i++] = hex[(gp->touchpad_x_1 >> 8) & 0xF];
            a_buf[i++] = hex[(gp->touchpad_x_1 >> 4) & 0xF];
            a_buf[i++] = hex[gp->touchpad_x_1 & 0xF];
            a_buf[i++] = ' '; a_buf[i++] = 'T'; a_buf[i++] = 'Y'; a_buf[i++] = ':';
            a_buf[i++] = hex[(gp->touchpad_y_1 >> 8) & 0xF];
            a_buf[i++] = hex[(gp->touchpad_y_1 >> 4) & 0xF];
            a_buf[i++] = hex[gp->touchpad_y_1 & 0xF];
            a_buf[i++] = '\0';
            
            g_vga->write(a_buf);
            g_vga->write("\n");
            
            // Copy state byte by byte to avoid operator= dependency if missing
            const uint8_t* src = reinterpret_cast<const uint8_t*>(gp);
            uint8_t* dst = reinterpret_cast<uint8_t*>(&last_gp_state);
            for (size_t i = 0; i < sizeof(Input::GamepadState); i++) dst[i] = src[i];
        }
    }
}

} // Input


void gamepad_thread_main() {
    while (1) {
        scheduler_block_current_thread(); 
        Input::GamepadManager::gamepad_debug_poll();
    }
}