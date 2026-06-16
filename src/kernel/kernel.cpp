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


#include <stdint.h>
#include <stddef.h>

#include "limine.h"
#include "module.hpp"
#include "limine_requests.hpp"
#include "vga.hpp"
#include "panic.hpp"
#include "serial.hpp"
#include "usb/usb.hpp"
#include "input/gamepad.hpp"

volatile struct limine_framebuffer_request g_framebuffer_request =
{
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

// Memory map: tells us which physical memory regions are usable
volatile struct limine_memmap_request g_memmap_request =
{
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

// Higher Half Direct Map: gives us the virtual offset to access physical memory
volatile struct limine_hhdm_request g_hhdm_request =
{
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

// ACPI RSDP: gives us the pointer to the ACPI tables
volatile struct limine_rsdp_request g_rsdp_request =
{
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

extern "C" [[noreturn]] void _start(void)
{
    // Execute all registered modules in order
    system_init_modules();

    // If framebuffer is not available, halt
    if (g_framebuffer_request.response == NULL || g_framebuffer_request.response->framebuffer_count < 1)
    {
        while (1) asm volatile ("hlt");
    }

    if (!g_vga) panic("VGA did not init");

    g_vga->write("Welcome to drakos!\n");
    
    // Main kernel loop: process deferred USB device initialization
    // and poll gamepad input. Runs with interrupts enabled (sti from GDT init).
    while (1) {
        // Process any USB devices that were registered during an interrupt
        if (g_usb_manager) g_usb_manager->update();
        
        // Print gamepad state changes
        static Input::GamepadState last_gp_state = {0};
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
            
            static uint32_t print_counter = 0;
            print_counter++;
            // Print every ~500 iterations depending on hlt
            if (print_counter > 500) {
                print_counter = 0;
                changed = true; // Force print periodically
            }

            if (changed) {
                g_vga->write("Gamepad 0: ");
                if (gp->btn_a) g_vga->write("CROSS ");
                if (gp->btn_b) g_vga->write("CIRCLE ");
                if (gp->btn_x) g_vga->write("SQUARE ");
                if (gp->btn_y) g_vga->write("TRIANGLE ");
                if (gp->dpad_up) g_vga->write("UP ");
                if (gp->dpad_down) g_vga->write("DOWN ");
                if (gp->dpad_left) g_vga->write("LEFT ");
                if (gp->dpad_right) g_vga->write("RIGHT ");
                
                // Print analog values
                char a_buf[32];
                // basic quick hex conversion for L2/R2
                const char* hex = "0123456789ABCDEF";
                a_buf[0] = ' '; a_buf[1] = 'L'; a_buf[2] = '2'; a_buf[3] = ':';
                a_buf[4] = hex[(gp->left_trigger >> 4) & 0xF];
                a_buf[5] = hex[gp->left_trigger & 0xF];
                a_buf[6] = ' '; a_buf[7] = 'R'; a_buf[8] = '2'; a_buf[9] = ':';
                a_buf[10] = hex[(gp->right_trigger >> 4) & 0xF];
                a_buf[11] = hex[gp->right_trigger & 0xF];
                a_buf[12] = '\0';
                g_vga->write(a_buf);
                g_vga->write("\n");
                
                // Copy state byte by byte to avoid operator= dependency if missing
                const uint8_t* src = reinterpret_cast<const uint8_t*>(gp);
                uint8_t* dst = reinterpret_cast<uint8_t*>(&last_gp_state);
                for (size_t i = 0; i < sizeof(Input::GamepadState); i++) {
                    dst[i] = src[i];
                }
            }
        }
        
        asm volatile("hlt"); // Sleep until next interrupt
    }
}