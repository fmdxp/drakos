#include "idt.hpp"

// ---------------------------------------------------------
// EXCEPTION HANDLERS (Using pure C++ with interrupt attribute)
// ---------------------------------------------------------

// Helper macro to define a standard exception handler without an error code
#define DEFINE_EXCEPTION_HANDLER(name, msg) \
    __attribute__((interrupt)) static void isr_##name(InterruptFrame* frame) { \
        (void)frame; \
        /* In a real OS, we would print the msg and halt or kill the process */ \
        while(1) asm volatile("cli; hlt"); \
    }

// Helper macro to define an exception handler WITH an error code
#define DEFINE_EXCEPTION_HANDLER_ERR(name, msg) \
    __attribute__((interrupt)) static void isr_##name(InterruptFrame* frame, uint64_t error_code) { \
        (void)frame; \
        (void)error_code; \
        while(1) asm volatile("cli; hlt"); \
    }

// CPU Exceptions (0 to 31)
DEFINE_EXCEPTION_HANDLER(divide_by_zero, "Divide By Zero")
DEFINE_EXCEPTION_HANDLER(debug, "Debug")
DEFINE_EXCEPTION_HANDLER(nmi, "Non-Maskable Interrupt")
DEFINE_EXCEPTION_HANDLER(breakpoint, "Breakpoint")
DEFINE_EXCEPTION_HANDLER(overflow, "Overflow")
DEFINE_EXCEPTION_HANDLER(bound_range_exceeded, "Bound Range Exceeded")
DEFINE_EXCEPTION_HANDLER(invalid_opcode, "Invalid Opcode")
DEFINE_EXCEPTION_HANDLER(device_not_available, "Device Not Available")
DEFINE_EXCEPTION_HANDLER_ERR(double_fault, "Double Fault")
DEFINE_EXCEPTION_HANDLER(coprocessor_segment_overrun, "Coprocessor Segment Overrun")
DEFINE_EXCEPTION_HANDLER_ERR(invalid_tss, "Invalid TSS")
DEFINE_EXCEPTION_HANDLER_ERR(segment_not_present, "Segment Not Present")
DEFINE_EXCEPTION_HANDLER_ERR(stack_segment_fault, "Stack-Segment Fault")
DEFINE_EXCEPTION_HANDLER_ERR(general_protection_fault, "General Protection Fault")
DEFINE_EXCEPTION_HANDLER_ERR(page_fault, "Page Fault")
DEFINE_EXCEPTION_HANDLER(reserved_15, "Reserved 15")
DEFINE_EXCEPTION_HANDLER(x87_floating_point, "x87 Floating-Point Exception")
DEFINE_EXCEPTION_HANDLER_ERR(alignment_check, "Alignment Check")
DEFINE_EXCEPTION_HANDLER(machine_check, "Machine Check")
DEFINE_EXCEPTION_HANDLER(simd_floating_point, "SIMD Floating-Point Exception")
DEFINE_EXCEPTION_HANDLER(virtualization, "Virtualization Exception")
DEFINE_EXCEPTION_HANDLER_ERR(control_protection, "Control Protection Exception")

// Generic handler for unhandled interrupts
__attribute__((interrupt)) static void isr_generic(InterruptFrame* frame) {
    (void)frame;
    while(1) asm volatile("cli; hlt");
}

// ---------------------------------------------------------
// IDT CLASS IMPLEMENTATION
// ---------------------------------------------------------

void IDT::set_entry(int index, uint64_t isr, uint8_t flags) {
    entries[index].isr_low = (uint16_t)isr;
    entries[index].kernel_cs = 0x08; // Kernel Code Segment from our GDT
    entries[index].ist = 0;
    entries[index].attributes = flags;
    entries[index].isr_mid = (uint16_t)(isr >> 16);
    entries[index].isr_high = (uint32_t)(isr >> 32);
    entries[index].reserved = 0;
}

bool IDT::start() {
    // Clear the IDT
    for (int i = 0; i < 256; i++) {
        set_entry(i, (uint64_t)isr_generic, 0x8E); // 0x8E = Present, Ring 0, Interrupt Gate
    }

    // Register CPU Exceptions
    set_entry(0, (uint64_t)isr_divide_by_zero, 0x8E);
    set_entry(1, (uint64_t)isr_debug, 0x8E);
    set_entry(2, (uint64_t)isr_nmi, 0x8E);
    set_entry(3, (uint64_t)isr_breakpoint, 0x8E);
    set_entry(4, (uint64_t)isr_overflow, 0x8E);
    set_entry(5, (uint64_t)isr_bound_range_exceeded, 0x8E);
    set_entry(6, (uint64_t)isr_invalid_opcode, 0x8E);
    set_entry(7, (uint64_t)isr_device_not_available, 0x8E);
    set_entry(8, (uint64_t)isr_double_fault, 0x8E);
    set_entry(9, (uint64_t)isr_coprocessor_segment_overrun, 0x8E);
    set_entry(10, (uint64_t)isr_invalid_tss, 0x8E);
    set_entry(11, (uint64_t)isr_segment_not_present, 0x8E);
    set_entry(12, (uint64_t)isr_stack_segment_fault, 0x8E);
    set_entry(13, (uint64_t)isr_general_protection_fault, 0x8E);
    set_entry(14, (uint64_t)isr_page_fault, 0x8E);
    set_entry(15, (uint64_t)isr_reserved_15, 0x8E);
    set_entry(16, (uint64_t)isr_x87_floating_point, 0x8E);
    set_entry(17, (uint64_t)isr_alignment_check, 0x8E);
    set_entry(18, (uint64_t)isr_machine_check, 0x8E);
    set_entry(19, (uint64_t)isr_simd_floating_point, 0x8E);
    set_entry(20, (uint64_t)isr_virtualization, 0x8E);
    set_entry(21, (uint64_t)isr_control_protection, 0x8E);

    // Setup pointer
    pointer.limit = (sizeof(IDTEntry) * 256) - 1;
    pointer.base = (uint64_t)&entries;

    // Load IDT
    asm volatile("lidt %0" : : "m"(pointer));

    // Enable interrupts
    asm volatile("sti");

    return true;
}

void IDT::stop() {
    // Disable interrupts
    asm volatile("cli");
}

const char* IDT::get_name() const {
    return "Interrupt Descriptor Table (IDT)";
}

// Register as Core Module
REGISTER_MODULE(g_idt, IDT, 1_core);
