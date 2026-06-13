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
};

} // namespace Input
