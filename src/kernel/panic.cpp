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

[[noreturn]] void panic(const char* message, InterruptFrame* frame) {
    // Disable interrupts to prevent nested panics
    asm volatile("cli");

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
        panic_print("----------------------\n");
    }

    panic_print("\nSystem Halted.");

    // Halt forever
    while (true) {
        asm volatile("hlt");
    }
}
