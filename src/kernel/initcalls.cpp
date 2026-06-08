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
}
