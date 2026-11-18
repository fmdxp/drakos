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


#pragma once

#include <stdint.h>
#include "kernel/handle.hpp"
#include "kernel/vm_region.hpp"

// Avx/Fpu State Size (2688 for AVX512, 512 for standard FXSAVE)
// We allocate enough space to safely use XSAVE if supported.
#define FPU_STATE_SIZE 4096

// CPU Register State saved during an interrupt or context switch
struct Context {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    // Interrupt frame
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

enum class ProcessState : uint8_t {
    Created,
    Initializing,
    Ready,
    Running,
    Terminating,
    Exited,
};

enum class ProcessResult : uint8_t {
    Success,
    InvalidArgument,
    InvalidState,
    OutOfMemory,
    AddressSpaceFailure,
    InUse,
    ReferenceOverflow,
    CleanupFailure,
};

class Process {
public:
    Process();
    static ProcessResult create_user(Process** output);

    bool retain();
    ProcessResult release();
    ProcessState state() const;
    bool transition_to(ProcessState next);
    ProcessResult finalize_termination();
    uint32_t reference_count() const;

    // Process ID
    uint64_t pid;
    
    // Physical page table (PML4 or PML5) assigned to this process
    uintptr_t page_table_phys;

    // Process is user-mode process?
    bool is_user;
    uint32_t scheduler_threads;
    uintptr_t next_user_stack_top;
    bool scheduler_cleanup_pending;
    Process* scheduler_next_pending;
    KernelHandleTable handles;
    VmRegionTable vm_regions;
    uintptr_t next_mmap_address;

private:
    struct UserProcessTag {};
    explicit Process(UserProcessTag);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool owns_address_space;
    volatile uint32_t references;
    volatile uint8_t lifecycle_state;

    ProcessResult destroy_resources();
    static bool valid_transition(ProcessState from, ProcessState to);
    friend struct ProcessLifecycleTestAccess;
};


enum ThreadState {
    THREAD_RUNNING,     // Thread is running
    THREAD_READY,       // Thread is ready to be run
    THREAD_SWITCHING,   // Saved context is not yet safe to run on another CPU
    THREAD_BLOCKED,     // Thread is suspended, waiting for an external event
    THREAD_SLEEPING,    // Thread is suspended, waiting for a specific time (es: sleep(1000);)
    THREAD_DEAD,        // Thread is killed or ended its execution, and it's waiting to be freed up
};



class Thread {
public:
    Thread(Process* parent, void (*entry_point)(), bool is_user, const char* const thread_name);
    ~Thread();

    // Thread ID
    uint64_t tid;

    // Parent process to which the thread belongs
    Process* parent_process;

    // Pointer to the saved context (at the top of the kernel stack)
    Context* context;

    // Base address of the stack in Kernel mode
    uintptr_t kernel_stack;
    uintptr_t kernel_stack_base;

    // Buffer for FPU/AVX512 state, 64-byte aligned
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(64)));

    // Thread state (e.g., Ready, Running, Blocked)
    ThreadState state;
    bool address_space_retained;
    bool scheduler_enrolled;
    bool process_final_thread;
    uint32_t last_cpu;
    uint32_t cpu_migrations;
    
    // Simple Round-Robin scheduler link
    Thread* next;
};

// Scheduler APIs
void scheduler_init();
void scheduler_start_application_processors();
void scheduler_activate_current_processor();
void scheduler_add_thread(Thread* thread);
extern "C" Context* scheduler_switch(Context* current_context);
extern "C" void scheduler_finish_switch();
void scheduler_block_current_thread();
void scheduler_wake_thread(Thread* t);
void scheduler_terminate_thread(Thread* t);
void scheduler_yield();
Thread* scheduler_get_current_thread();
uint32_t scheduler_count_running_process_cpus(const Process* process);
uint32_t scheduler_count_process_threads(const Process* process);
uint32_t scheduler_enabled_processor_count();
uint32_t scheduler_thread_migration_count(const Thread* thread);
