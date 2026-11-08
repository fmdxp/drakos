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
ENTRY(kernel_main)

SECTIONS
{
    . = 0xffffffff80000000;

    .text : ALIGN(4K) {
        *(.text .text.*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
        
        /* --- GLOBAL CONSTRUCTORS / DESTRUCTORS --- */
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(.init_array))
        KEEP(*(.init_array.*))
        KEEP(*(.ctors))
        __init_array_end = .;

        . = ALIGN(8);
        __fini_array_start = .;
        KEEP(*(.fini_array))
        KEEP(*(.fini_array.*))
        KEEP(*(.dtors))
        __fini_array_end = .;

        /* Immutable initcall descriptors, ordered by their phase suffix. */
        . = ALIGN(8);
        __initcalls_start = .;
        KEEP(*(SORT_BY_NAME(.initcalls.*)))
        __initcalls_end = .;
    }

    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    .bss : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    }
}
