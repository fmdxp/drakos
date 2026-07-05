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


#include "input/gamepad.hpp"
#include "vga.hpp"

namespace Input {

GamepadState GamepadManager::gamepads[4] = {0};

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

} // namespace Input
