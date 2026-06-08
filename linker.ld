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