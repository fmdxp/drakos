OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0xffffffff80000000;

    .text : ALIGN(4K) {
        *(.text .text.*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
        /* --- MODULE INITCALLS --- */
        . = ALIGN(8);
        __initcalls_1_core_start = .;
        KEEP(*(.initcalls.1_core))
        __initcalls_1_core_end = .;

        /* --- MEMORY SUBSYSTEM (ordered: PMM → VMM → Heap) --- */
        . = ALIGN(8);
        __initcalls_2_mem_a_start = .;
        KEEP(*(.initcalls.2_mem_a))
        __initcalls_2_mem_a_end = .;

        . = ALIGN(8);
        __initcalls_2_mem_b_start = .;
        KEEP(*(.initcalls.2_mem_b))
        __initcalls_2_mem_b_end = .;

        . = ALIGN(8);
        __initcalls_2_mem_c_start = .;
        KEEP(*(.initcalls.2_mem_c))
        __initcalls_2_mem_c_end = .;

        . = ALIGN(8);
        __initcalls_3_drv_start = .;
        KEEP(*(.initcalls.3_drv))
        __initcalls_3_drv_end = .;

        /* --- DEVICE DRIVERS (Keyboard, Mouse, etc) --- */
        . = ALIGN(8);
        __initcalls_4_dev_start = .;
        KEEP(*(.initcalls.4_dev))
        __initcalls_4_dev_end = .;
    }

    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    .bss : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    }
}