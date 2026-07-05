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
#include "lapic.hpp"
#include "vga.hpp"
#include "panic.hpp"
#include "thread.hpp" // for Context

// External assembly stubs
extern "C" void isr_0();
extern "C" void isr_1();
extern "C" void isr_2();
extern "C" void isr_3();
extern "C" void isr_4();
extern "C" void isr_5();
extern "C" void isr_6();
extern "C" void isr_7();
extern "C" void isr_8();
extern "C" void isr_9();
extern "C" void isr_10();
extern "C" void isr_11();
extern "C" void isr_12();
extern "C" void isr_13();
extern "C" void isr_14();
extern "C" void isr_15();
extern "C" void isr_16();
extern "C" void isr_17();
extern "C" void isr_18();
extern "C" void isr_19();
extern "C" void isr_20();
extern "C" void isr_21();

// Generate an array of extern C function pointers for 22..255
// Since we can't easily loop extern declarations in C++, we'll just map the ones we need.
// Wait, we defined isr_22 to isr_255 in assembly using .altmacro. We can declare them as an array if we export them.
// But we don't need to put them in the IDT unless we want to catch them.

extern "C" void xhci_handle_interrupt();

extern "C" void keyboard_isr();
extern void lapic_eoi();

extern "C" void isr_32();
extern "C" void isr_33();
extern "C" void isr_40();

// The central C++ interrupt handler called by isr_stubs.S
extern "C" void isr_handler(Context* ctx) {
    if (ctx->int_no < 32) {
        // Exception
        if (g_vga) {
            g_vga->write("\nException Number: ");
            char buf[16];
            int n = ctx->int_no;
            int i = 0;
            if (n == 0) buf[i++] = '0';
            while(n > 0) { buf[i++] = (n % 10) + '0'; n /= 10; }
            for(int j=0; j<i/2; j++) { char t=buf[j]; buf[j]=buf[i-1-j]; buf[i-1-j]=t; }
            buf[i] = '\0';
            g_vga->write(buf);
            g_vga->write("\n");
        }
        panic("Unhandled CPU Exception", (InterruptFrame*)&ctx->rip);
    }
    
    // Example hardware interrupts
    if (ctx->int_no == 32) {
        // APIC Timer (IRQ 0 via APIC)
        // We just send EOI. The context switch will happen automatically
        // when isr_common calls scheduler_switch.
        lapic_eoi();
    } else if (ctx->int_no == 33) {
        // Keyboard (IRQ 1)
        keyboard_isr();
        lapic_eoi(); // If using APIC. If PIC, outb(0x20, 0x20)
    } else if (ctx->int_no == 40) {
        // xHCI (IRQ 8 mapped)
        xhci_handle_interrupt();
        lapic_eoi();
    } else {
        lapic_eoi();
    }
}

bool IDT::start() {
    pointer.limit = (sizeof(IDTEntry) * 256) - 1;
    pointer.base = (uint64_t)&entries;

    for (int i = 0; i < 256; i++) {
        set_entry(i, 0, 0x8E); // Dummy init
    }

    set_entry(0,  (uint64_t)isr_0,  0x8E);
    set_entry(1,  (uint64_t)isr_1,  0x8E);
    set_entry(2,  (uint64_t)isr_2,  0x8E);
    set_entry(3,  (uint64_t)isr_3,  0x8E);
    set_entry(4,  (uint64_t)isr_4,  0x8E);
    set_entry(5,  (uint64_t)isr_5,  0x8E);
    set_entry(6,  (uint64_t)isr_6,  0x8E);
    set_entry(7,  (uint64_t)isr_7,  0x8E);
    set_entry(8,  (uint64_t)isr_8,  0x8E);
    set_entry(9,  (uint64_t)isr_9,  0x8E);
    set_entry(10, (uint64_t)isr_10, 0x8E);
    set_entry(11, (uint64_t)isr_11, 0x8E);
    set_entry(12, (uint64_t)isr_12, 0x8E);
    set_entry(13, (uint64_t)isr_13, 0x8E);
    set_entry(14, (uint64_t)isr_14, 0x8E);
    set_entry(15, (uint64_t)isr_15, 0x8E);
    set_entry(16, (uint64_t)isr_16, 0x8E);
    set_entry(17, (uint64_t)isr_17, 0x8E);
    set_entry(18, (uint64_t)isr_18, 0x8E);
    set_entry(19, (uint64_t)isr_19, 0x8E);
    set_entry(20, (uint64_t)isr_20, 0x8E);
    set_entry(21, (uint64_t)isr_21, 0x8E);
    
    set_entry(32, (uint64_t)isr_32, 0x8E);
    set_entry(33, (uint64_t)isr_33, 0x8E);
    set_entry(40, (uint64_t)isr_40, 0x8E);

    // TODO: Cleanly register all 256 from an array instead of manual externs

    asm volatile("lidt %0" : : "m"(pointer));
    asm volatile("sti");

    return true;
}

void IDT::stop() {
    asm volatile("cli");
}

const char* IDT::get_name() const {
    return "Interrupt Descriptor Table (IDT)";
}

void IDT::set_entry(int vector, uint64_t handler, uint8_t flags) {
    entries[vector].isr_low = handler & 0xFFFF;
    entries[vector].kernel_cs = 0x08; // Kernel Code Segment
    entries[vector].ist = 0;
    entries[vector].attributes = flags;
    entries[vector].isr_mid = (handler >> 16) & 0xFFFF;
    entries[vector].isr_high = (handler >> 32) & 0xFFFFFFFF;
    entries[vector].reserved = 0;
}

REGISTER_MODULE(g_idt, IDT, 1_core);
