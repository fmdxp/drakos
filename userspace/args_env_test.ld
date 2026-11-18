OUTPUT_FORMAT(elf64-x86-64)
ENTRY(user_args_test_start)

SECTIONS
{
    . = 0x400000;
    .text : ALIGN(4K) { *(.text .text.*) }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }

    . = 0x500000;
    .data : ALIGN(4K) { *(.data .data.*) }
    .bss : ALIGN(4K) { *(.bss .bss.*) *(COMMON) }
}
