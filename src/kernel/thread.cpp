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


#include "thread.hpp"
#include "vmm.hpp"
#include "pmm.hpp"
#include "gdt.hpp"
#include <stddef.h>

extern GDT* g_gdt;

// Global pointers
static Thread* s_current_thread     = nullptr;
static Thread* s_idle_thread        = nullptr;
static Thread* s_last_normal_thread = nullptr;


static void idle_main() {
    while (1) asm volatile("hlt");
}


Process::Process() {
    static uint64_t next_pid = 1;
    pid = next_pid++;
    is_user = false;
    // Default: use the kernel master PML4.
    // User processes will override this in the Thread constructor.
    page_table_phys = vmm_get_kernel_pml4();
}

Process::~Process() {
}

Thread::Thread(Process* parent, void (*entry_point)(), bool is_user, [[maybe_unused]] const char* const thread_name) {
    static uint64_t next_tid = 1;
    tid = next_tid++;
    parent_process = parent;
    state = THREAD_READY; // Ready
    
    // Allocate a 16KB kernel stack
    uint8_t* stack = new uint8_t[16384];
    
    // The top of the stack (stack grows downwards)
    uint64_t stack_top = (uint64_t)stack + 16384;
    
    // kernel_stack stores the TOP of the kernel stack (for TSS RSP0 and for the scheduler)
    kernel_stack = stack_top;
    
    // Reserve space for the Context structure at the top of the stack
    context = reinterpret_cast<Context*>(stack_top - sizeof(Context));
    
    // Zero out the Context
    uint8_t* ctx_ptr = reinterpret_cast<uint8_t*>(context);
    for (size_t i = 0; i < sizeof(Context); i++) ctx_ptr[i] = 0;
    
    // Initialize FPU state to 0
    for (size_t i = 0; i < 4096; i++) fpu_state[i] = 0;
    
    // Initialize the Interrupt Frame for IRETQ
    if (is_user) {
        context->cs = 0x20 | 3; // USER_CS with RPL 3
        context->ss = 0x18 | 3; // USER_SS with RPL 3
        context->rflags = 0x202; // IF (Interrupts enabled)
        
        // Give this user process its own isolated address space
        parent_process->is_user = true;
        parent_process->page_table_phys = vmm_create_address_space();
        
        // Allocate User Stack (1 Page = 4KB) at 0x00007FFFF0000000
        uintptr_t user_stack_phys = pmm_alloc_page();
        uintptr_t user_stack_virt = 0x00007FFFF0000000 - 4096;
        vmm_map_page(parent_process->page_table_phys, user_stack_virt, user_stack_phys, VMM_PRESENT | VMM_WRITE | VMM_USER);
        
        context->rsp = 0x00007FFFF0000000;
    } else {
        context->cs = 0x08; // KERNEL_CS
        context->ss = 0x10; // KERNEL_SS
        context->rflags = 0x202; // IF (Interrupts enabled)
        // For kernel threads, RSP points to just above the Context struct on the stack
        context->rsp = stack_top;
    }
    
    context->rip = (uint64_t)entry_point;
    
    // By default, a thread points to itself in the circular list
    next = this;
}

Thread::~Thread() {
    // In a real OS we would delete the kernel stack here
}

void scheduler_init() {
    // We create a dummy "Idle/Boot" thread to represent the currently executing kernel code
    Process* kernel_proc = new Process();
    Thread* boot_thread = new Thread(kernel_proc, nullptr, false, (char*)"core");
    
    s_current_thread = boot_thread;
    s_last_normal_thread = boot_thread;
    s_idle_thread = new Thread(kernel_proc, idle_main, false, (char*)"idle");
}

void scheduler_add_thread(Thread* thread) {
    if (!s_current_thread) {
        s_current_thread = thread;
    } else {
        // Insert into the circular linked list
        Thread* next_node = s_current_thread->next;
        s_current_thread->next = thread;
        thread->next = next_node;
    }
}

// Low-level XSAVE / XRSTOR wrappers
static inline void arch_save_fpu(void* buffer) {
    asm volatile("xsave64 %0" : "=m"(*(uint8_t*)buffer) : "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
}

static inline void arch_restore_fpu(void* buffer) {
    asm volatile("xrstor64 %0" : : "m"(*(uint8_t*)buffer), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
}

extern "C" Context* scheduler_switch(Context* current_context) {
    if (!s_current_thread) return current_context;
    
    // 1. Save state of the current thread
    s_current_thread->context = current_context;
    arch_save_fpu(s_current_thread->fpu_state);
    
    if (s_current_thread->state == THREAD_RUNNING)
        s_current_thread->state = THREAD_READY;


    Thread* start_thread;
    if (s_current_thread == s_idle_thread) start_thread = s_last_normal_thread;
    else start_thread = s_current_thread;
    if (!start_thread) start_thread = s_current_thread;


    // 2. Pick the next thread in the Round-Robin queue
    Thread* next_thread = start_thread->next;
    bool found = false;

    while (next_thread != start_thread)
    {
        if (next_thread->state == THREAD_READY) {
            found = true;
            break;
        }
        next_thread = next_thread->next;
    }
    
    if (!found && start_thread->state == THREAD_READY){
        next_thread = start_thread;
        found = true;
    }

    if (found) {
        s_current_thread = next_thread;
        s_last_normal_thread = s_current_thread;
    }

    else s_current_thread = s_idle_thread;
    

    s_current_thread->state = THREAD_RUNNING;
    arch_restore_fpu(s_current_thread->fpu_state);
    
    // Switch Page Table (PML4) if parent_process changed!
    if (s_current_thread->parent_process) {
        vmm_switch_address_space(s_current_thread->parent_process->page_table_phys);
    }
    
    // Update TSS (Task State Segment) RSP0 so Ring 3 interrupts land on the new Kernel Stack
    if (g_gdt) {
        g_gdt->set_kernel_stack(s_current_thread->kernel_stack);
    }
    
    return s_current_thread->context;
}

void scheduler_yield() {
    asm volatile("int $32");   // Interrupt 32 -> Triggers timer
}

void scheduler_block_current_thread() {
    s_current_thread->state = THREAD_BLOCKED;
    scheduler_yield();
}

void scheduler_wake_thread(Thread* t) {
    if (t) t->state = THREAD_READY;
}

Thread* scheduler_get_current_thread() {
    return s_current_thread;
}