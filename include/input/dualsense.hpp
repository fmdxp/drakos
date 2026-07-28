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
#include "input/gamepad.hpp"

#include "input/hid_transport.hpp"

class DualSenseDriver : public Input::GamepadDriver {
public:
    DualSenseDriver(HIDTransport* transport);
    
    // Called by the transport layer when a HID report arrives
    void on_report_received(const uint8_t* report, size_t length);
    
    void process_input() override;
    
    // Set the RGB color of the Lightbar
    void set_led(uint8_t r, uint8_t g, uint8_t b);
    void init_lightbar();


private:
    HIDTransport* m_transport;
    
    bool parse_report(const uint8_t* report);
};

extern DualSenseDriver* g_dualsense_driver;
