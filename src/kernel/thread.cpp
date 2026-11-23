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
#include "kernel/smp.hpp"
#include "kernel/panic.hpp"
#include "cpu.hpp"
#include "drivers/lapic.hpp"
#include <stddef.h>

extern GDT* g_gdt;

static constexpr uint32_t SCHEDULER_MAX_CPUS = 256;
struct CpuSchedulerState {
    Thread* current;
    Thread* idle;
    Thread* retired;
    Thread* pending_ready;
    bool first_dispatch;
    bool enabled;
};
static CpuSchedulerState s_cpu_scheduler[SCHEDULER_MAX_CPUS] = {};
static Thread* s_run_queue = nullptr;
static Thread* s_run_cursor = nullptr;
static uint32_t s_runnable_count = 0;
static Process* s_kernel_process = nullptr;
static Process* s_pending_process_cleanup = nullptr;
static volatile bool s_scheduler_lock = false;
static volatile bool s_fail_next_scheduler_publication = false;
static volatile bool s_fail_next_user_stack_allocation = false;
static volatile bool s_fail_next_user_stack_mapping = false;
static volatile bool s_fail_next_thread_initialization = false;

static void enqueue_process_cleanup_locked(Process* process) {
    if (!process || process->scheduler_cleanup_pending) return;
    if (!process->scheduler_cleanup_reference_held) {
        if (!process->retain()) panic("Process cleanup ownership could not be retained");
        process->scheduler_cleanup_reference_held = true;
    }
    process->scheduler_cleanup_pending = true;
    process->scheduler_next_pending = s_pending_process_cleanup;
    s_pending_process_cleanup = process;
}

static uint64_t scheduler_lock() {
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&s_scheduler_lock, __ATOMIC_ACQUIRE)) asm volatile("pause");
    return flags;
}

static void scheduler_unlock(uint64_t flags) {
    __atomic_clear(&s_scheduler_lock, __ATOMIC_RELEASE);
    if (flags & 0x200) asm volatile("sti" ::: "memory");
}

static CpuSchedulerState* current_cpu_state() {
    uint32_t index = smp_current_processor_index();
    return index < SCHEDULER_MAX_CPUS ? &s_cpu_scheduler[index] : nullptr;
}


static void idle_main() {
    while (1) asm volatile("hlt");
}


Thread::Thread(Process* parent, void (*entry_point)(), bool is_user,
               [[maybe_unused]] const char* const thread_name,
               const UserProcessArguments* arguments) {
    static uint64_t next_tid = 1;
    tid = __atomic_add_fetch(&next_tid, 1, __ATOMIC_RELAXED) - 1;
    parent_process = parent;
    address_space_retained = false;
    scheduler_enrolled = false;
    process_final_thread = false;
    wait_wakeup_pending = false;
    waiting_on_process = nullptr;
    last_cpu = UINT32_MAX;
    cpu_migrations = 0;
    state = THREAD_READY; // Ready
    initialization_result = ThreadInitResult::Success;
    stack_mapping_error = VmmMapResult::Success;
    stack_region_error = VmRegionResult::Success;
    user_stack_error = UserStackBuildResult::Success;
    kernel_stack_base = 0;
    kernel_stack = 0;
    context = nullptr;

    if (!parent_process || !parent_process->retain()) {
        parent_process = nullptr;
        state = THREAD_DEAD;
        initialization_result = ThreadInitResult::InvalidProcess;
        return;
    }
    ProcessState process_state = parent_process->state();
    if ((process_state != ProcessState::Ready && process_state != ProcessState::Running) ||
        (is_user && (!parent_process->is_user || !parent_process->page_table_phys))) {
        state = THREAD_DEAD;
        initialization_result = ThreadInitResult::InvalidProcess;
        return;
    }
    if (is_user && !entry_point) {
        state = THREAD_DEAD;
        initialization_result = ThreadInitResult::InvalidEntryPoint;
        return;
    }
    
    // Allocate a 16KB kernel stack
    uint8_t* stack = new uint8_t[16384];
    if (!stack) {
        state = THREAD_DEAD;
        initialization_result = ThreadInitResult::KernelStackOutOfMemory;
        return;
    }
    kernel_stack_base = reinterpret_cast<uintptr_t>(stack);
    
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

    if (__atomic_exchange_n(&s_fail_next_thread_initialization, false, __ATOMIC_ACQ_REL)) {
        state = THREAD_DEAD;
        initialization_result = ThreadInitResult::InjectedFailure;
        return;
    }
    
    // Initialize the Interrupt Frame for IRETQ
    if (is_user) {
        context->cs = 0x20 | 3; // USER_CS with RPL 3
        context->ss = 0x18 | 3; // USER_SS with RPL 3
        context->rflags = 0x202; // IF (Interrupts enabled)
        
        // Give this user process its own isolated address space
        // Reserve a unique one-page stack slot with a one-page unmapped gap.
        uintptr_t user_stack_phys = 0;
        if (!__atomic_exchange_n(&s_fail_next_user_stack_allocation, false, __ATOMIC_ACQ_REL))
            user_stack_phys = pmm_alloc_page();
        if (!user_stack_phys) {
            state = THREAD_DEAD;
            initialization_result = ThreadInitResult::UserStackOutOfMemory;
            return;
        }
        uintptr_t user_stack_top = __atomic_load_n(&parent_process->next_user_stack_top,
                                                    __ATOMIC_RELAXED);
        uintptr_t next_stack_top = 0;
        for (;;) {
            constexpr uintptr_t USER_LIMIT = 1ULL << 47;
            if ((user_stack_top & (PAGE_SIZE - 1)) || user_stack_top > USER_LIMIT ||
                user_stack_top < 2 * PAGE_SIZE) {
                pmm_free_page(user_stack_phys);
                state = THREAD_DEAD;
                initialization_result = ThreadInitResult::UserStackOutOfMemory;
                return;
            }
            next_stack_top = user_stack_top - 2 * PAGE_SIZE;
            if (__atomic_compare_exchange_n(&parent_process->next_user_stack_top,
                                            &user_stack_top, next_stack_top, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                break;
        }
        uintptr_t user_stack_virt = user_stack_top - PAGE_SIZE;
        auto* stack_bytes = reinterpret_cast<uint8_t*>(user_stack_phys + pmm_hhdm_offset());
        for (size_t i = 0; i < PAGE_SIZE; ++i) stack_bytes[i] = 0;
        uint64_t stack_flags = VMM_PRESENT | VMM_WRITE | VMM_USER;
        if (nx_enabled()) stack_flags |= VMM_NX;
        if (__atomic_exchange_n(&s_fail_next_user_stack_mapping, false, __ATOMIC_ACQ_REL))
            stack_mapping_error = VmmMapResult::PageTableOutOfMemory;
        else
            stack_mapping_error = vmm_map_owned_page(parent_process->page_table_phys,
                user_stack_virt, user_stack_phys, stack_flags);
        if (stack_mapping_error != VmmMapResult::Success) {
            pmm_free_page(user_stack_phys);
            state = THREAD_DEAD;
            initialization_result = ThreadInitResult::UserStackMappingFailed;
            return;
        }
        VmRegion stack_region{static_cast<uint8_t>(VM_PERM_READ | VM_PERM_WRITE),
            VmRegionType::Stack, VmRegionSharing::Private, 0,
            user_stack_virt, user_stack_virt + PAGE_SIZE, 0, nullptr};
        stack_region_error = parent_process->vm_regions.insert(stack_region);
        if (stack_region_error != VmRegionResult::Success) {
            if (vmm_unmap_page(parent_process->page_table_phys, user_stack_virt) !=
                VmmUnmapResult::Success)
                panic("Failed to roll back an unpublished user stack mapping");
            state = THREAD_DEAD;
            initialization_result = ThreadInitResult::UserStackRegionFailed;
            return;
        }

        const UserProcessArguments empty_arguments{};
        uintptr_t initial_stack_pointer = 0;
        user_stack_error = user_stack_build(stack_bytes, PAGE_SIZE, user_stack_virt,
            arguments ? *arguments : empty_arguments, &initial_stack_pointer);
        if (user_stack_error != UserStackBuildResult::Success) {
            state = THREAD_DEAD;
            initialization_result =
                user_stack_error == UserStackBuildResult::InvalidArguments
                    ? ThreadInitResult::InvalidUserArguments
                    : ThreadInitResult::UserArgumentsTooLarge;
            return;
        }
        context->rsp = initial_stack_pointer;
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
    if (kernel_stack_base) {
        delete[] reinterpret_cast<uint8_t*>(kernel_stack_base);
        kernel_stack_base = 0;
    }
    if (parent_process) {
        Process* process = parent_process;
        parent_process = nullptr;
        ProcessResult result = process->release();
        if (result != ProcessResult::Success)
            panic("Thread could not release its process reference");
    }
}

void scheduler_init() {
    s_kernel_process = new Process();
    if (!s_kernel_process) return;
    uint32_t bsp = smp_current_processor_index();
    if (bsp >= SCHEDULER_MAX_CPUS) return;
    Thread* boot_thread = new Thread(s_kernel_process, nullptr, false, "core");
    Thread* bsp_idle = new Thread(s_kernel_process, idle_main, false, "idle-bsp");
    if (!boot_thread || !bsp_idle || boot_thread->state == THREAD_DEAD ||
        bsp_idle->state == THREAD_DEAD) return;

    s_run_queue = boot_thread;
    s_run_cursor = boot_thread;
    s_runnable_count = 1;
    boot_thread->next = boot_thread;
    boot_thread->state = THREAD_RUNNING;
    s_kernel_process->scheduler_threads = 2;
    s_cpu_scheduler[bsp].current = boot_thread;
    s_cpu_scheduler[bsp].idle = bsp_idle;
    s_cpu_scheduler[bsp].enabled = true;

    for (uint32_t cpu = 0; cpu < SCHEDULER_MAX_CPUS; ++cpu) {
        if (cpu == bsp || !smp_processor_online(cpu)) continue;
        Thread* idle = new Thread(s_kernel_process, idle_main, false, "idle-ap");
        if (!idle || idle->state == THREAD_DEAD) return;
        s_cpu_scheduler[cpu].current = idle;
        s_cpu_scheduler[cpu].idle = idle;
        s_cpu_scheduler[cpu].first_dispatch = true;
        ++s_kernel_process->scheduler_threads;
    }
}

void scheduler_start_application_processors() {
    if (!smp_start_application_processors())
        panic("Unable to start application processor schedulers");
}

void scheduler_activate_current_processor() {
    CpuSchedulerState* cpu = current_cpu_state();
    if (cpu && cpu->current && cpu->idle) cpu->enabled = true;
}

void thread_test_fail_next_user_stack_allocation() {
    __atomic_store_n(&s_fail_next_user_stack_allocation, true, __ATOMIC_RELEASE);
}

void thread_test_fail_next_user_stack_mapping() {
    __atomic_store_n(&s_fail_next_user_stack_mapping, true, __ATOMIC_RELEASE);
}

void thread_test_fail_next_initialization() {
    __atomic_store_n(&s_fail_next_thread_initialization, true, __ATOMIC_RELEASE);
}

void scheduler_test_fail_next_publication() {
    __atomic_store_n(&s_fail_next_scheduler_publication, true, __ATOMIC_RELEASE);
}

SchedulerPublishResult scheduler_add_thread(Thread* thread) {
    if (!thread) return SchedulerPublishResult::InvalidThread;
    if (thread->scheduler_enrolled) return SchedulerPublishResult::AlreadyPublished;
    if (thread->state == THREAD_DEAD || !thread->context || !thread->kernel_stack_base)
        return SchedulerPublishResult::InvalidThread;
    if (__atomic_exchange_n(&s_fail_next_scheduler_publication, false, __ATOMIC_ACQ_REL))
        return SchedulerPublishResult::InjectedFailure;
    Process* process = thread->parent_process;
    if (!process || (process->state() != ProcessState::Ready &&
                     process->state() != ProcessState::Running)) {
        return SchedulerPublishResult::InvalidProcess;
    }
    if (process->is_user) {
        if (vmm_retain_address_space(process->page_table_phys) != VmmAddressSpaceResult::Success) {
            return SchedulerPublishResult::AddressSpaceFailure;
        }
        thread->address_space_retained = true;
    }
    uint64_t flags = scheduler_lock();
    ProcessState process_state = process->state();
    if ((process_state != ProcessState::Ready && process_state != ProcessState::Running) ||
        !s_run_queue) {
        scheduler_unlock(flags);
        if (thread->address_space_retained) {
            if (vmm_release_address_space(process->page_table_phys) !=
                VmmAddressSpaceResult::Success)
                panic("Scheduler could not roll back a thread address-space reference");
            thread->address_space_retained = false;
        }
        return SchedulerPublishResult::SchedulerUnavailable;
    }
    Thread* tail = s_run_cursor ? s_run_cursor : s_run_queue;
    thread->next = tail->next;
    tail->next = thread;
    s_run_cursor = thread;
    ++s_runnable_count;
    thread->scheduler_enrolled = true;
    if (process) ++process->scheduler_threads;
    scheduler_unlock(flags);
    return SchedulerPublishResult::Success;
}

// Low-level XSAVE / XRSTOR wrappers
static inline void arch_save_fpu(void* buffer) {
    asm volatile("xsave64 %0" : "=m"(*(uint8_t*)buffer) : "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
}

static inline void arch_restore_fpu(void* buffer) {
    asm volatile("xrstor64 %0" : : "m"(*(uint8_t*)buffer), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
}

extern "C" Context* scheduler_switch(Context* current_context) {
    // TLB and scheduler-start IPIs are service interrupts, not scheduling
    // points. In particular, a TLB responder must return immediately after
    // acknowledgement so it never waits on the VMM lock held by the sender.
    if (current_context && current_context->int_no == 241) return current_context;
    CpuSchedulerState* cpu = current_cpu_state();
    if (!cpu || !cpu->enabled || !cpu->current || !cpu->idle || !current_context)
        return current_context;
    uint64_t flags = scheduler_lock();

    Thread* retired = cpu->retired;
    cpu->retired = nullptr;
    Thread* old_thread = cpu->current;
    if (!cpu->first_dispatch) {
        old_thread->context = current_context;
        arch_save_fpu(old_thread->fpu_state);
        cpu->pending_ready = old_thread;
        if (old_thread->state == THREAD_RUNNING) old_thread->state = THREAD_SWITCHING;
        else if (old_thread->state == THREAD_BLOCKING)
            old_thread->state = THREAD_WAIT_SWITCH;
    } else {
        cpu->first_dispatch = false;
    }

    Thread* next_thread = nullptr;
    if (s_run_queue) {
        Thread* cursor = s_run_cursor ? s_run_cursor->next : s_run_queue;
        Thread* first = cursor;
        do {
            if (cursor->state == THREAD_READY) {
                next_thread = cursor;
                break;
            }
            cursor = cursor->next;
        } while (cursor && cursor != first);
    }
    if (next_thread) {
        uint32_t cpu_index = smp_current_processor_index();
        if (next_thread->last_cpu != UINT32_MAX && next_thread->last_cpu != cpu_index)
            ++next_thread->cpu_migrations;
        next_thread->last_cpu = cpu_index;
        next_thread->state = THREAD_RUNNING;
        s_run_cursor = next_thread;
        cpu->current = next_thread;
    } else {
        cpu->idle->state = THREAD_RUNNING;
        cpu->current = cpu->idle;
    }

    Process* selected_process = cpu->current->parent_process;
    bool selected_process_ready = !selected_process ||
        selected_process->transition_to(ProcessState::Running);

    // Remove all dead threads from the shared run queue while transitions are
    // serialized. Frame/reference operations happen after releasing this lock.
    Thread* dead_threads = nullptr;
    Thread* live_head = nullptr;
    Thread* live_tail = nullptr;
    Thread* cursor = s_run_queue;
    Thread* old_cursor = s_run_cursor;
    uint32_t old_count = s_runnable_count;
    s_run_queue = nullptr;
    s_run_cursor = nullptr;
    s_runnable_count = 0;
    for (uint32_t i = 0; cursor && i < old_count; ++i) {
        Thread* item = cursor;
        cursor = item->next;
        bool executing_on_cpu = false;
        if (item->state == THREAD_DEAD) {
            for (uint32_t cpu_index = 0; cpu_index < SCHEDULER_MAX_CPUS; ++cpu_index) {
                if (s_cpu_scheduler[cpu_index].current == item ||
                    s_cpu_scheduler[cpu_index].pending_ready == item) {
                    executing_on_cpu = true;
                    break;
                }
            }
        }
        if (item->state == THREAD_DEAD && !executing_on_cpu) {
            item->next = dead_threads;
            dead_threads = item;
            continue;
        }
        if (!live_head) {
            live_head = live_tail = item;
            item->next = item;
        } else {
            item->next = live_head;
            live_tail->next = item;
            live_tail = item;
        }
        ++s_runnable_count;
        if (item == old_cursor) s_run_cursor = item;
    }
    s_run_queue = live_head;
    if (s_run_cursor == nullptr) s_run_cursor = live_tail;

    for (Thread* item = dead_threads; item; item = item->next) {
        Process* process = item->parent_process;
        if (process && process->scheduler_threads) --process->scheduler_threads;
        item->process_final_thread = process && process->scheduler_threads == 0;
        if (item->process_final_thread) {
            ProcessState process_state = process->state();
            if ((process_state != ProcessState::Terminating &&
                 !process->transition_to(ProcessState::Terminating)) || !process->retain())
                panic("Final process thread could not retain terminal cleanup ownership");
            process->scheduler_cleanup_reference_held = true;
        }
        item->scheduler_enrolled = false;
    }

    Thread* selected = cpu->current;
    uintptr_t selected_root = selected->parent_process
        ? selected->parent_process->page_table_phys : vmm_get_kernel_pml4();
    Context* selected_context = selected->context;
    scheduler_unlock(flags);
    if (!selected_process_ready) panic("Scheduler selected a process that is not ready");

    // Never spin with interrupts disabled on the VMM lock here: its owner may
    // be waiting for this CPU to acknowledge a TLB IPI. Mask the local timer,
    // briefly allow the high-priority shootdown IPI through, and retry.
    uint32_t saved_timer = g_lapic ? g_lapic->read(LAPIC_LVT_TMR) : 0;
    if (g_lapic) g_lapic->write(LAPIC_LVT_TMR, saved_timer | (1U << 16));
    uint32_t saved_tpr = lapic_lock_wait_begin();
    for (;;) {
        VmmAddressSpaceResult switch_result = vmm_try_switch_address_space(selected_root);
        if (switch_result == VmmAddressSpaceResult::Success) break;
        if (switch_result != VmmAddressSpaceResult::Busy)
            panic("Scheduler selected an invalid address space");
        asm volatile("sti; pause; cli" ::: "memory");
    }
    lapic_lock_wait_end(saved_tpr);
    if (g_lapic) g_lapic->write(LAPIC_LVT_TMR, saved_timer);
    smp_set_kernel_stack(selected->kernel_stack);
    if (g_gdt) g_gdt->set_kernel_stack(selected->kernel_stack);
    arch_restore_fpu(selected->fpu_state);

    // Reap only after releasing the run-queue lock and moving to a different
    // address space/stack. Heap reclamation may itself perform VMM shootdowns.
    while (retired) {
        Thread* item = retired;
        retired = item->next;
        item->next = nullptr;
        delete item;
    }

    while (dead_threads) {
        Thread* item = dead_threads;
        dead_threads = item->next;
        Process* process = item->parent_process;
        if (item->address_space_retained && process) {
            VmmAddressSpaceResult release_result = vmm_release_address_space(process->page_table_phys);
            if (release_result != VmmAddressSpaceResult::Success)
                panic("Unable to release thread address-space reference");
            item->address_space_retained = false;
        }
        if (item->process_final_thread) {
            uint64_t cleanup_flags = scheduler_lock();
            enqueue_process_cleanup_locked(process);
            scheduler_unlock(cleanup_flags);
        }
        uint64_t retire_flags = scheduler_lock();
        item->next = cpu->retired;
        cpu->retired = item;
        scheduler_unlock(retire_flags);
    }

    // Address-space destruction can transiently report ActiveAddressSpace if a
    // peer has published its new scheduler state but has not finished CR3 yet.
    // Retry on later timer entries after this CPU has completed its own switch.
    uint64_t cleanup_flags = scheduler_lock();
    Process* pending = s_pending_process_cleanup;
    s_pending_process_cleanup = nullptr;
    for (Process* item = pending; item; item = item->scheduler_next_pending)
        item->scheduler_cleanup_pending = false;
    scheduler_unlock(cleanup_flags);
    while (pending) {
        Process* process = pending;
        pending = process->scheduler_next_pending;
        process->scheduler_next_pending = nullptr;
        ProcessResult result = process->finalize_termination();
        if (result == ProcessResult::Success) {
            uint64_t release_flags = scheduler_lock();
            process->scheduler_cleanup_reference_held = false;
            scheduler_unlock(release_flags);
            if (process->release() != ProcessResult::Success)
                panic("Scheduler could not release terminal process ownership");
        } else {
            uint64_t retry_flags = scheduler_lock();
            enqueue_process_cleanup_locked(process);
            scheduler_unlock(retry_flags);
        }
    }
    return selected_context;
}

extern "C" void scheduler_finish_switch() {
    CpuSchedulerState* cpu = current_cpu_state();
    if (!cpu) return;
    uint64_t flags = scheduler_lock();
    Thread* previous = cpu->pending_ready;
    cpu->pending_ready = nullptr;
    if (previous && previous->state == THREAD_SWITCHING)
        previous->state = THREAD_READY;
    else if (previous && previous->state == THREAD_WAIT_SWITCH) {
        previous->state = previous->wait_wakeup_pending ? THREAD_READY : THREAD_BLOCKED;
        previous->wait_wakeup_pending = false;
    }
    scheduler_unlock(flags);
}

void scheduler_yield() {
    asm volatile("int $32");   // Interrupt 32 -> Triggers timer
}

void scheduler_block_current_thread() {
    Thread* current = scheduler_get_current_thread();
    if (!current) return;
    uint64_t flags = scheduler_lock();
    if (current->state == THREAD_RUNNING) current->state = THREAD_BLOCKED;
    scheduler_unlock(flags);
    scheduler_yield();
}

void scheduler_wake_thread(Thread* t) {
    if (!t) return;
    uint64_t flags = scheduler_lock();
    if (t->scheduler_enrolled) {
        if (t->state == THREAD_BLOCKING || t->state == THREAD_WAIT_SWITCH)
            t->wait_wakeup_pending = true;
        else if (t->state == THREAD_BLOCKED)
            t->state = THREAD_READY;
    }
    scheduler_unlock(flags);
}

bool scheduler_prepare_wait_current_thread(Process* wait_owner) {
    if (!wait_owner) return false;
    uint64_t flags = scheduler_lock();
    CpuSchedulerState* cpu = current_cpu_state();
    Thread* current = cpu ? cpu->current : nullptr;
    bool prepared = current && current->scheduler_enrolled &&
        current->state == THREAD_RUNNING && current->parent_process &&
        current->parent_process->is_user;
    if (prepared) {
        current->waiting_on_process = wait_owner;
        current->wait_wakeup_pending = false;
        current->state = THREAD_BLOCKING;
    }
    scheduler_unlock(flags);
    return prepared;
}

void scheduler_clear_current_thread_wait() {
    uint64_t flags = scheduler_lock();
    CpuSchedulerState* cpu = current_cpu_state();
    Thread* current = cpu ? cpu->current : nullptr;
    if (current) {
        current->waiting_on_process = nullptr;
        current->wait_wakeup_pending = false;
    }
    scheduler_unlock(flags);
}

void scheduler_wake_process_waiters(Process* wait_owner) {
    if (!wait_owner) return;
    uint64_t flags = scheduler_lock();
    if (s_run_queue) {
        Thread* thread = s_run_queue;
        uint32_t remaining = s_runnable_count;
        while (remaining-- && thread) {
            if (thread->scheduler_enrolled && thread->waiting_on_process == wait_owner) {
                if (thread->state == THREAD_BLOCKING || thread->state == THREAD_WAIT_SWITCH)
                    thread->wait_wakeup_pending = true;
                else if (thread->state == THREAD_BLOCKED)
                    thread->state = THREAD_READY;
            }
            thread = thread->next;
        }
    }
    scheduler_unlock(flags);
}

void scheduler_terminate_thread(Thread* t) {
    if (!t) return;
    uint64_t flags = scheduler_lock();
    if (t->scheduler_enrolled) t->state = THREAD_DEAD;
    scheduler_unlock(flags);
}

ProcessTerminationRequestResult scheduler_terminate_process(Process* process, int32_t status) {
    bool reschedule[SCHEDULER_MAX_CPUS] = {};
    uint32_t current_cpu = smp_current_processor_index();
    uint64_t flags = scheduler_lock();
    if (!process || !process->is_user) {
        scheduler_unlock(flags);
        return ProcessTerminationRequestResult::InvalidProcess;
    }

    ProcessState state = process->state();
    bool first_request = state == ProcessState::Ready || state == ProcessState::Running;
    if (first_request) {
        // The scheduler lock serializes competing exits with thread selection
        // and publication. Store status before exposing Terminating.
        __atomic_store_n(&process->stored_exit_status, status, __ATOMIC_RELAXED);
        __atomic_store_n(&process->explicit_exit_status, true, __ATOMIC_RELEASE);
        if (!process->transition_to(ProcessState::Terminating)) {
            scheduler_unlock(flags);
            return ProcessTerminationRequestResult::InvalidState;
        }
    } else if (state != ProcessState::Terminating) {
        scheduler_unlock(flags);
        return ProcessTerminationRequestResult::InvalidState;
    }

    for (Thread* thread = s_run_queue, *first = s_run_queue;
         thread; thread = thread->next) {
        if (thread->parent_process == process) thread->state = THREAD_DEAD;
        if (!thread->next || thread->next == first) break;
    }
    for (uint32_t index = 0; index < SCHEDULER_MAX_CPUS; ++index) {
        CpuSchedulerState& target = s_cpu_scheduler[index];
        if (index != current_cpu && target.enabled && target.current &&
            target.current->parent_process == process)
            reschedule[index] = true;
    }
    if (process->scheduler_threads == 0 &&
        !process->scheduler_cleanup_reference_held && !process->scheduler_cleanup_pending)
        enqueue_process_cleanup_locked(process);
    scheduler_unlock(flags);

    for (uint32_t index = 0; index < SCHEDULER_MAX_CPUS; ++index) {
        if (reschedule[index] && !smp_request_reschedule(index))
            panic("Unable to reschedule a CPU running an exiting process");
    }
    return first_request ? ProcessTerminationRequestResult::Success
                         : ProcessTerminationRequestResult::AlreadyTerminating;
}

ProcessExitRequestResult scheduler_exit_current_process(int32_t status) {
    uint64_t flags = scheduler_lock();
    CpuSchedulerState* cpu = current_cpu_state();
    Thread* caller = cpu ? cpu->current : nullptr;
    Process* process = caller ? caller->parent_process : nullptr;
    bool valid_thread = caller && caller->scheduler_enrolled;
    scheduler_unlock(flags);
    if (!valid_thread) return ProcessExitRequestResult::InvalidThread;
    if (!process || !process->is_user) return ProcessExitRequestResult::InvalidProcess;

    ProcessTerminationRequestResult result = scheduler_terminate_process(process, status);
    if (result == ProcessTerminationRequestResult::Success)
        return ProcessExitRequestResult::Success;
    if (result == ProcessTerminationRequestResult::AlreadyTerminating)
        return ProcessExitRequestResult::AlreadyTerminating;
    return ProcessExitRequestResult::InvalidProcess;
}

Thread* scheduler_get_current_thread() {
    CpuSchedulerState* cpu = current_cpu_state();
    return cpu ? cpu->current : nullptr;
}

uint32_t scheduler_count_running_process_cpus(const Process* process) {
    if (!process) return 0;
    uint64_t flags = scheduler_lock();
    uint32_t count = 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_CPUS; ++i) {
        Thread* current = s_cpu_scheduler[i].current;
        if (current && current->parent_process == process && current->state == THREAD_RUNNING)
            ++count;
    }
    scheduler_unlock(flags);
    return count;
}

uint32_t scheduler_count_process_threads(const Process* process) {
    if (!process) return 0;
    uint64_t flags = scheduler_lock();
    uint32_t count = process->scheduler_threads;
    scheduler_unlock(flags);
    return count;
}

uint32_t scheduler_count_process_waiters(const Process* process) {
    if (!process) return 0;
    uint64_t flags = scheduler_lock();
    uint32_t count = 0;
    if (s_run_queue) {
        Thread* thread = s_run_queue;
        uint32_t remaining = s_runnable_count;
        while (remaining-- && thread) {
            if (thread->scheduler_enrolled && thread->waiting_on_process == process &&
                (thread->state == THREAD_WAIT_SWITCH || thread->state == THREAD_BLOCKED))
                ++count;
            thread = thread->next;
        }
    }
    scheduler_unlock(flags);
    return count;
}

uint32_t scheduler_enabled_processor_count() {
    uint64_t flags = scheduler_lock();
    uint32_t count = 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_CPUS; ++i)
        if (s_cpu_scheduler[i].enabled) ++count;
    scheduler_unlock(flags);
    return count;
}

uint32_t scheduler_thread_migration_count(const Thread* thread) {
    if (!thread) return 0;
    uint64_t flags = scheduler_lock();
    uint32_t count = thread->cpu_migrations;
    scheduler_unlock(flags);
    return count;
}
