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


#include <stdint.h>
#include <stddef.h>
 
#include "kernel.hpp"
#include "module.hpp"

#include "limine_requests.hpp"
#include "limine.h"

#include "vga.hpp"
#include "serial.hpp"
#include "panic.hpp"
#include "usb.hpp"
#include "gamepad.hpp"

#include "thread.hpp"
#include "vfs.hpp"

#include "pmm.hpp"
#include "vmm.hpp"

#include "syscalls.hpp"
#include "cpu.hpp"
#include "boot_diagnostics.hpp"
#include "address_space_runtime_test.hpp"
#include "kernel/smp.hpp"

static void make_threads_and_processes()
{
    Process* usb_proc = new Process();
    if (usb_proc) {
        Thread* thread = new Thread(usb_proc, usb_thread_main, false, "USB Controller");
        if (thread && thread->initialization_result == ThreadInitResult::Success &&
            scheduler_add_thread(thread) == SchedulerPublishResult::Success)
            g_usb_thread = thread;
        else {
            delete thread;
            g_usb_thread = nullptr;
        }
        if (usb_proc->release() != ProcessResult::Success)
            panic("Unable to release USB worker process owner reference");
    }


    Process* gamepad_proc = new Process();
    if (gamepad_proc) {
        Thread* thread = new Thread(gamepad_proc, gamepad_thread_main, false, "Gamepad Debugger");
        if (thread && thread->initialization_result == ThreadInitResult::Success &&
            scheduler_add_thread(thread) == SchedulerPublishResult::Success)
            g_gamepad_thread = thread;
        else {
            delete thread;
            g_gamepad_thread = nullptr;
        }
        if (gamepad_proc->release() != ProcessResult::Success)
            panic("Unable to release gamepad worker process owner reference");
    }

    // g_kernel_thread = scheduler_get_current_thread();
}


static bool command_line_has_test(const char* command_line) {
    constexpr char requested[] = "drakos.test=address-space";
    if (!command_line) return false;
    for (const char* at = command_line; *at; ++at) {
        size_t i = 0;
        while (requested[i] && at[i] == requested[i]) ++i;
        if (!requested[i]) return true;
    }
    return false;
}

extern "C" [[noreturn]] void kernel_main(void)
{
    // 0. Call C++ global constructors BEFORE anything else!    (i actually forgot in older commits...)
    cpp_call_constructors();

    const char* command_line = nullptr;
    if (g_cmdline_request.response && g_cmdline_request.response->cmdline) {
        command_line = g_cmdline_request.response->cmdline;
    }
    boot_diagnostics_init(command_line);
    // NX must be enabled before the VMM publishes any NX leaf entries.
    (void)enable_nx();

    // Initialize ordered kernel components. A mandatory failure prevents normal operation.
    InitcallSummary initcalls = system_init_modules();
    if (initcalls.mandatory_failures != 0) {
        panic("Required kernel initialization failed");
    }
    
    // 2. Enable MSR Syscalls (yeee)
    enable_syscalls();

    // 3. Enable XSAVE and AVX only AFTER checking CPUID
    enable_fpu_and_avx();
    
    // Start AP service loops first, then initialize each per-CPU run-queue
    // state and release APs into the shared scheduler.
    if (!smp_initialize()) panic("SMP initialization failed");
    scheduler_init();
    if (command_line_has_test(command_line)) run_address_space_runtime_test();
    scheduler_start_application_processors();
    if (command_line_has_test(command_line)) {
        if (!run_smp_kernel_scheduler_runtime_test())
            panic("SMP kernel scheduler runtime test failed");
        if (!run_smp_user_scheduler_runtime_test())
            panic("SMP user scheduler runtime test failed");
        if (!run_smp_independent_user_runtime_test())
            panic("SMP independent address-space runtime test failed");
    }
    
    // 5. Make all the kernel threads and processes
    make_threads_and_processes();


    // Kernel is now initialized!
    if (g_vga) g_vga->write("Welcome to drakos!\n");
    // test_vfs();

    
    

    // Main Kernel Loop
    while (1) {
        scheduler_block_current_thread();
    }
}
