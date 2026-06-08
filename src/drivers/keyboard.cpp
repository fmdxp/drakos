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


#include "keyboard.hpp"
#include "idt.hpp"
#include "lapic.hpp"
#include "ioapic.hpp"
#include "vga.hpp"

// Utility to read from an I/O port
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// Basic Scancode Set 1 to ASCII map (lowercase only for now)
static const char scancode_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', /* 9 */
  '9', '0', '-', '=', '\b', /* Backspace */
  '\t',     /* Tab */
  'q', 'w', 'e', 'r',  /* 19 */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', /* Enter key */
    0,      /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', /* 39 */
 '\'', '`',   0,      /* Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',      /* 49 */
  'm', ',', '.', '/',   0,      /* Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
    0,  /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,  /* < ... F10 */
    0,  /* 69 - Num lock*/
    0,  /* Scroll Lock */
    0,  /* Home key */
    0,  /* Up Arrow */
    0,  /* Page Up */
  '-',
    0,  /* Left Arrow */
    0,
    0,  /* Right Arrow */
  '+',
    0,  /* 79 - End key*/
    0,  /* Down Arrow */
    0,  /* Page Down */
    0,  /* Insert Key */
    0,  /* Delete Key */
    0,   0,   0,
    0,  /* F11 Key */
  0, /* F12 Key */
  0, /* All other keys are undefined */
};

// Scancode Set 1 to ASCII map (with Shift held down)
static const char scancode_map_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', /* 9 */
  '(', ')', '_', '+', '\b', /* Backspace */
  '\t',     /* Tab */
  'Q', 'W', 'E', 'R',  /* 19 */
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', /* Enter key */
    0,      /* 29   - Control */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', /* 39 */
 '\"', '~',   0,      /* Left shift */
 '|', 'Z', 'X', 'C', 'V', 'B', 'N',      /* 49 */
  'M', '<', '>', '?',   0,      /* Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
    0,  /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,  /* < ... F10 */
    0,  /* 69 - Num lock*/
    0,  /* Scroll Lock */
    0,  /* Home key */
    0,  /* Up Arrow */
    0,  /* Page Up */
  '-',
    0,  /* Left Arrow */
    0,
    0,  /* Right Arrow */
  '+',
    0,  /* 79 - End key*/
    0,  /* Down Arrow */
    0,  /* Page Down */
    0,  /* Insert Key */
    0,  /* Delete Key */
    0,   0,   0,
    0,  /* F11 Key */
    0,  /* F12 Key */
    0, /* All other keys are undefined */
};

// Modifiers State
static bool s_shift_pressed = false;
static bool s_caps_lock_active = false;

extern "C" __attribute__((interrupt)) void keyboard_isr(InterruptFrame* frame) {
    (void)frame;

    // Read the scancode from PS/2 data port
    uint8_t scancode = inb(0x60);

    // If the top bit is set, it's a key release event (break code).
    bool is_release = (scancode & 0x80) != 0;
    uint8_t key = scancode & 0x7F;

    if (is_release) {
        // Handle shift release
        if (key == 0x2A || key == 0x36) {
            s_shift_pressed = false;
        }
    } else {
        // Handle shift press
        if (key == 0x2A || key == 0x36) {
            s_shift_pressed = true;
        } 
        // Handle caps lock toggle
        else if (key == 0x3A) {
            s_caps_lock_active = !s_caps_lock_active;
        } 
        // Normal keys
        else if (key < 128) {
            char c = 0;
            
            // Determine if letter should be uppercase
            bool is_letter = (scancode_map[key] >= 'a' && scancode_map[key] <= 'z');
            bool shift_effect = s_shift_pressed;
            
            if (is_letter && s_caps_lock_active) {
                shift_effect = !shift_effect; // Caps lock reverses shift for letters
            }

            if (shift_effect) {
                c = scancode_map_shift[key];
            } else {
                c = scancode_map[key];
            }

            if (c) {
                if (g_vga) {
                    g_vga->write_char(c);
                }
            }
        }
    }

    // Acknowledge the interrupt so the Local APIC can send more
    if (g_lapic) {
        g_lapic->eoi();
    }
}

bool Keyboard::start() {
    // Route IRQ 1 (Keyboard) to IDT Vector 33
    if (g_ioapic) {
        g_ioapic->set_entry(1, 33);
    }
    return true;
}

void Keyboard::stop() {
}

const char* Keyboard::get_name() const {
    return "PS/2 Keyboard";
}

// We register this at level 4, after APIC and IDT are initialized
REGISTER_MODULE(g_keyboard, Keyboard, 4_dev);
