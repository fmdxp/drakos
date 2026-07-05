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


#pragma once
#include <stdint.h>

namespace Input {

enum class GamepadType {
    Unknown,
    PlayStation4,
    PlayStation5,
    Xbox,
    ProController
};

struct GamepadState {
    bool connected;
    GamepadType type;
    
    // Buttons
    bool dpad_up;
    bool dpad_down;
    bool dpad_left;
    bool dpad_right;
    
    bool btn_a; // Cross on PS, A on Xbox
    bool btn_b; // Circle on PS, B on Xbox
    bool btn_x; // Square on PS, X on Xbox
    bool btn_y; // Triangle on PS, Y on Xbox
    
    bool btn_l1;
    bool btn_r1;
    bool btn_l3;
    bool btn_r3;
    
    bool btn_options;
    bool btn_share;
    bool btn_logo;
    
    // Analog Sticks (0 to 255, center is 128)
    uint8_t left_stick_x;
    uint8_t left_stick_y;
    uint8_t right_stick_x;
    uint8_t right_stick_y;
    
    // Analog Triggers (0 to 255)
    uint8_t left_trigger;
    uint8_t right_trigger;
    
    // Touchpad
    bool btn_touchpad;
    
    bool touchpad_touching_1;
    uint16_t touchpad_x_1;
    uint16_t touchpad_y_1;
    
    bool touchpad_touching_2;
    uint16_t touchpad_x_2;
    uint16_t touchpad_y_2;
};

class GamepadDriver {
protected:
    int m_gamepad_index = -1;
public:
    virtual ~GamepadDriver() = default;
    virtual void process_input() = 0;
};

class GamepadManager {
public:
    static GamepadState gamepads[4];
    
    // Assegna il primo slot libero a un nuovo gamepad
    static int register_gamepad(GamepadType type);
    
    static GamepadState* get_gamepad(int index) {
        if (index >= 0 && index < 4) return &gamepads[index];
        return nullptr;
    }

    static void gamepad_debug_poll();
};

} // namespace Input

void gamepad_thread_main();
