#include "module.hpp"

// Extern symbols from linker script
extern "C" initcall_t __initcalls_1_core_start[];
extern "C" initcall_t __initcalls_1_core_end[];
extern "C" initcall_t __initcalls_2_mem_start[];
extern "C" initcall_t __initcalls_2_mem_end[];
extern "C" initcall_t __initcalls_3_drv_start[];
extern "C" initcall_t __initcalls_3_drv_end[];

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
    execute_initcalls(__initcalls_2_mem_start, __initcalls_2_mem_end);
    execute_initcalls(__initcalls_3_drv_start, __initcalls_3_drv_end);
}
