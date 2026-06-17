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


#include "module.hpp"

// Extern symbols from linker script
extern "C" initcall_t __initcalls_1_core_start[];
extern "C" initcall_t __initcalls_1_core_end[];
extern "C" initcall_t __initcalls_2_mem_a_start[];  // PMM
extern "C" initcall_t __initcalls_2_mem_a_end[];
extern "C" initcall_t __initcalls_2_mem_b_start[];  // VMM
extern "C" initcall_t __initcalls_2_mem_b_end[];
extern "C" initcall_t __initcalls_2_mem_c_start[];  // Heap
extern "C" initcall_t __initcalls_2_mem_c_end[];
extern initcall_t __initcalls_3_drv_start[];
extern initcall_t __initcalls_3_drv_end[];

extern initcall_t __initcalls_4_dev_start[];
extern initcall_t __initcalls_4_dev_end[];

extern initcall_t __initcalls_5_usb_start[];
extern initcall_t __initcalls_5_usb_end[];

static void execute_initcalls(initcall_t* start, initcall_t* end) {
    for (initcall_t* call = start; call < end; call++) {
        bool success = (*call)();
        if (!success) {
            // Module failed and detached itself. 
            // In the future, we could log this to the screen.
        }
    }
}

void system_init_modules() {
    execute_initcalls(__initcalls_1_core_start, __initcalls_1_core_end);
    execute_initcalls(__initcalls_2_mem_a_start, __initcalls_2_mem_a_end); // PMM first
    execute_initcalls(__initcalls_2_mem_b_start, __initcalls_2_mem_b_end);
    execute_initcalls(__initcalls_2_mem_c_start, __initcalls_2_mem_c_end);
    execute_initcalls(__initcalls_3_drv_start, __initcalls_3_drv_end);
    execute_initcalls(__initcalls_4_dev_start, __initcalls_4_dev_end);
    execute_initcalls(__initcalls_5_usb_start, __initcalls_5_usb_end);
}
