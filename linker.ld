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

        . = ALIGN(8);
        __initcalls_2_mem_start = .;
        KEEP(*(.initcalls.2_mem))
        __initcalls_2_mem_end = .;

        . = ALIGN(8);
        __initcalls_3_drv_start = .;
        KEEP(*(.initcalls.3_drv))
        __initcalls_3_drv_end = .;
    }

    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    .bss : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    }
}