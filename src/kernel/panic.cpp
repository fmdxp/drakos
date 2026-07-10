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


#include "panic.hpp"
#include "vga.hpp"
#include "serial.hpp"

// Helper to write to both VGA and Serial
static void panic_print(const char* str) {
    
    if (g_vga) g_vga->write(str);
    if (g_serial) g_serial->write(str);
}

// Helper to print hex values
static void panic_print_hex(uint64_t val, int digits) {
    panic_print("0x");
    for (int i = digits - 1; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0x0F;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        
        if (g_vga) g_vga->write_char(c);
        if (g_serial) g_serial->write_char(c);
    }
}

static volatile int s_panic_lock = 0;

[[noreturn]] void panic(const char* message, InterruptFrame* frame) {
    // Disable interrupts immediately
    asm volatile("cli");

    // Spin-acquire the panic lock.
    // If another CPU already panicked, just halt — don't print twice.
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_panic_lock, &expected, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) while (true) asm volatile("hlt");
    

    panic_print("\n\n=======================================\n");
    panic_print("             KERNEL PANIC\n");
    panic_print("=======================================\n");
    panic_print("Reason: ");
    panic_print(message);
    panic_print("\n\n");

    if (frame) {
        panic_print("--- CPU State Dump ---\n");
        panic_print("RIP: "); panic_print_hex(frame->rip, 16);
        panic_print("  CS: "); panic_print_hex(frame->cs, 4); panic_print("\n");
        
        panic_print("RSP: "); panic_print_hex(frame->rsp, 16);
        panic_print("  SS: "); panic_print_hex(frame->ss, 4); panic_print("\n");
        
        panic_print("RFLAGS: "); panic_print_hex(frame->rflags, 16); panic_print("\n");
        
        // Print CR2 (faulting address) — invaluable for page faults
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        panic_print("CR2 (fault addr): "); panic_print_hex(cr2, 16); panic_print("\n");
        panic_print("----------------------\n");
    }

    panic_print("\nSystem Halted.");

    // Halt forever
    while (true) {
        asm volatile("hlt");
    }
}
