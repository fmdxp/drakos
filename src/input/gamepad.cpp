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
