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


#include "idt.hpp"

// We declare the keyboard ISR here as an extern for now, or we can just provide a generic one.
// Actually, it's better to expose a way to register interrupt handlers dynamically.
// For now, we will declare the keyboard_isr here.
extern "C" __attribute__((interrupt)) void keyboard_isr(InterruptFrame* frame);

// Generic ISR for unhandled interrupts
__attribute__((interrupt)) void default_isr(InterruptFrame* frame) {
    (void)frame;
    // We could log or halt here.
}

bool IDT::start() {
    // 1. Initialize all IDT entries to the default ISR
    for (int i = 0; i < 256; i++) {
        set_entry(i, reinterpret_cast<uint64_t>(default_isr), 0x8E); // 0x8E: Present, Ring 0, Interrupt Gate
    }

    // 2. Set the Keyboard ISR at Vector 33
    set_entry(33, reinterpret_cast<uint64_t>(keyboard_isr), 0x8E);

    // 3. Setup the IDT pointer
    pointer.limit = sizeof(entries) - 1;
    pointer.base = reinterpret_cast<uint64_t>(&entries[0]);

    // 4. Load the IDT
    asm volatile("lidt %0" : : "m"(pointer));

    // 5. Enable hardware interrupts!
    asm volatile("sti");

    return true;
}

void IDT::stop() {
    asm volatile("cli");
}

const char* IDT::get_name() const {
    return "IDT";
}

void IDT::set_entry(int index, uint64_t isr, uint8_t flags) {
    IDTEntry* entry = &entries[index];
    
    entry->isr_low = isr & 0xFFFF;
    entry->kernel_cs = 0x08; // Kernel Code Segment (0x08)
    entry->ist = 0;
    entry->attributes = flags;
    entry->isr_mid = (isr >> 16) & 0xFFFF;
    entry->isr_high = (isr >> 32) & 0xFFFFFFFF;
    entry->reserved = 0;
}

// Register as a core driver (must be loaded before standard drivers like keyboard)
REGISTER_MODULE(g_idt, IDT, 3_drv);
