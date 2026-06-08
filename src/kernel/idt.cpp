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
#include "panic.hpp"

// We declare the keyboard ISR here as an extern for now, or we can just provide a generic one.
// Actually, it's better to expose a way to register interrupt handlers dynamically.
// For now, we will declare the keyboard_isr here.
extern "C" __attribute__((interrupt)) void keyboard_isr(InterruptFrame* frame);
extern "C" __attribute__((interrupt)) void xhci_isr(InterruptFrame* frame);

// Generic ISR for unhandled interrupts
__attribute__((interrupt)) void default_isr(InterruptFrame* frame) {
    (void)frame;
    // We could log or halt here.
}

// Exception handlers
#define ISR_NO_ERR(name, msg) \
    __attribute__((interrupt)) void isr_##name(InterruptFrame* frame) { \
        panic(msg, frame); \
    }

#define ISR_ERR(name, msg) \
    __attribute__((interrupt)) void isr_##name(InterruptFrame* frame, uint64_t error_code) { \
        (void)error_code; \
        panic(msg, frame); \
    }

ISR_NO_ERR(0,  "Divide by Zero")
ISR_NO_ERR(1,  "Debug")
ISR_NO_ERR(2,  "Non-Maskable Interrupt")
ISR_NO_ERR(3,  "Breakpoint")
ISR_NO_ERR(4,  "Overflow")
ISR_NO_ERR(5,  "Bound Range Exceeded")
ISR_NO_ERR(6,  "Invalid Opcode")
ISR_NO_ERR(7,  "Device Not Available")
ISR_ERR   (8,  "Double Fault")
ISR_NO_ERR(9,  "Coprocessor Segment Overrun")
ISR_ERR   (10, "Invalid TSS")
ISR_ERR   (11, "Segment Not Present")
ISR_ERR   (12, "Stack-Segment Fault")
ISR_ERR   (13, "General Protection Fault")
ISR_ERR   (14, "Page Fault")
ISR_NO_ERR(15, "Reserved")
ISR_NO_ERR(16, "x87 Floating-Point Exception")
ISR_ERR   (17, "Alignment Check")
ISR_NO_ERR(18, "Machine Check")
ISR_NO_ERR(19, "SIMD Floating-Point Exception")
ISR_NO_ERR(20, "Virtualization Exception")
ISR_ERR   (21, "Control Protection Exception")

bool IDT::start() {
    // 1. Initialize all IDT entries to the default ISR
    for (int i = 0; i < 256; i++) {
        set_entry(i, reinterpret_cast<uint64_t>(default_isr), 0x8E); // 0x8E: Present, Ring 0, Interrupt Gate
    }

    // 2. Map the CPU Exceptions (0-31)
    set_entry(0,  reinterpret_cast<uint64_t>(isr_0),  0x8E);
    set_entry(1,  reinterpret_cast<uint64_t>(isr_1),  0x8E);
    set_entry(2,  reinterpret_cast<uint64_t>(isr_2),  0x8E);
    set_entry(3,  reinterpret_cast<uint64_t>(isr_3),  0x8E);
    set_entry(4,  reinterpret_cast<uint64_t>(isr_4),  0x8E);
    set_entry(5,  reinterpret_cast<uint64_t>(isr_5),  0x8E);
    set_entry(6,  reinterpret_cast<uint64_t>(isr_6),  0x8E);
    set_entry(7,  reinterpret_cast<uint64_t>(isr_7),  0x8E);
    set_entry(8,  reinterpret_cast<uint64_t>(isr_8),  0x8E);
    set_entry(9,  reinterpret_cast<uint64_t>(isr_9),  0x8E);
    set_entry(10, reinterpret_cast<uint64_t>(isr_10), 0x8E);
    set_entry(11, reinterpret_cast<uint64_t>(isr_11), 0x8E);
    set_entry(12, reinterpret_cast<uint64_t>(isr_12), 0x8E);
    set_entry(13, reinterpret_cast<uint64_t>(isr_13), 0x8E);
    set_entry(14, reinterpret_cast<uint64_t>(isr_14), 0x8E);
    set_entry(15, reinterpret_cast<uint64_t>(isr_15), 0x8E);
    set_entry(16, reinterpret_cast<uint64_t>(isr_16), 0x8E);
    set_entry(17, reinterpret_cast<uint64_t>(isr_17), 0x8E);
    set_entry(18, reinterpret_cast<uint64_t>(isr_18), 0x8E);
    set_entry(19, reinterpret_cast<uint64_t>(isr_19), 0x8E);
    set_entry(20, reinterpret_cast<uint64_t>(isr_20), 0x8E);
    set_entry(21, reinterpret_cast<uint64_t>(isr_21), 0x8E);

    // 2. Set the Keyboard ISR at Vector 33
    set_entry(33, reinterpret_cast<uint64_t>(keyboard_isr), 0x8E);

    // 3. Set the xHCI ISR at Vector 40
    set_entry(40, reinterpret_cast<uint64_t>(xhci_isr), 0x8E);

    // 4. Setup the IDT pointer
    pointer.limit = sizeof(entries) - 1;
    pointer.base = reinterpret_cast<uint64_t>(&entries[0]);

    // 5. Load the IDT
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
