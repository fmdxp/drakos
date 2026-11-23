#include "kernel/address_space_runtime_test.hpp"

#include "kernel/boot_diagnostics.hpp"
#include "kernel/thread.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"
#include "cpu.hpp"

extern "C" const uint8_t process_termination_blob_start[];
extern "C" const uint8_t process_termination_blob_end[];
extern "C" const uint8_t process_termination_parent[];
extern "C" const uint8_t process_termination_child_a[];
extern "C" const uint8_t process_termination_child_b[];

namespace {
constexpr uintptr_t kTerminationCodeVa = 0x00400000;
constexpr uintptr_t kTerminationDataVa = 0x00401000;

void clear_termination_frame(uintptr_t frame) {
    auto* bytes = reinterpret_cast<uint8_t*>(frame + pmm_hhdm_offset());
    for (size_t i = 0; i < PAGE_SIZE; ++i) bytes[i] = 0;
}

bool map_termination_pages(Process* process, uintptr_t code, uintptr_t data) {
    uint64_t data_flags = VMM_PRESENT | VMM_USER | VMM_WRITE;
    if (nx_enabled()) data_flags |= VMM_NX;
    return vmm_map_page(process->page_table_phys, kTerminationCodeVa, code,
                        VMM_PRESENT | VMM_USER) == VmmMapResult::Success &&
           vmm_map_page(process->page_table_phys, kTerminationDataVa, data,
                        data_flags) == VmmMapResult::Success;
}

bool termination_wait_exited(Process* process) {
    for (size_t attempt = 0; attempt < 10000 &&
         process->state() != ProcessState::Exited; ++attempt) scheduler_yield();
    return process->state() == ProcessState::Exited;
}
}

bool run_process_termination_runtime_test() {
    uintptr_t code_frame = pmm_alloc_page();
    uintptr_t data_frame = pmm_alloc_page();
    if (!code_frame || !data_frame) {
        if (code_frame) pmm_free_page(code_frame);
        if (data_frame) pmm_free_page(data_frame);
        boot_diagnostics_write("controlled process termination: FAIL (page allocation)\n");
        return false;
    }
    clear_termination_frame(code_frame);
    clear_termination_frame(data_frame);
    auto* code = reinterpret_cast<uint8_t*>(code_frame + pmm_hhdm_offset());
    size_t code_size = static_cast<size_t>(process_termination_blob_end -
                                           process_termination_blob_start);
    for (size_t i = 0; i < code_size; ++i) code[i] = process_termination_blob_start[i];

    Process* parent = nullptr;
    Process* child = nullptr;
    Process* empty_child = nullptr;
    bool processes_ready = Process::create_user(&parent) == ProcessResult::Success && parent &&
        Process::create_user(&child) == ProcessResult::Success && child &&
        Process::create_user(&empty_child) == ProcessResult::Success && empty_child;
    bool linked = processes_ready && map_termination_pages(parent, code_frame, data_frame) &&
        map_termination_pages(child, code_frame, data_frame) &&
        map_termination_pages(empty_child, code_frame, data_frame) &&
        process_attach_child(parent, child) == ProcessRelationshipResult::Success &&
        process_attach_child(parent, empty_child) == ProcessRelationshipResult::Success;
    if (!linked) {
        if (parent) (void)parent->release();
        if (child) (void)child->release();
        if (empty_child) (void)empty_child->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("controlled process termination: FAIL (process setup)\n");
        return false;
    }

    auto* shared = reinterpret_cast<volatile uint64_t*>(data_frame + pmm_hhdm_offset());
    shared[0] = child->pid;
    shared[2] = empty_child->pid;
    shared[7] = UINT64_MAX; // A PID that cannot belong to this parent.
    ProcessTerminationRequestResult empty_result = process_terminate_child(parent,
        empty_child->pid, 37);
    if (empty_result != ProcessTerminationRequestResult::Success) {
        (void)parent->release(); (void)child->release(); (void)empty_child->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("controlled process termination: FAIL (empty child request)\n");
        return false;
    }
    auto entry = [](const uint8_t* symbol) {
        return reinterpret_cast<void(*)()>(kTerminationCodeVa +
            static_cast<uintptr_t>(symbol - process_termination_blob_start));
    };
    Thread* parent_thread = new Thread(parent, entry(process_termination_parent), true,
                                        "controlled-termination-parent");
    Thread* child_a = new Thread(child, entry(process_termination_child_a), true,
                                  "termination-target-a");
    Thread* child_b = new Thread(child, entry(process_termination_child_b), true,
                                  "termination-target-b");
    if (!parent_thread || !child_a || !child_b || parent_thread->state == THREAD_DEAD ||
        child_a->state == THREAD_DEAD || child_b->state == THREAD_DEAD) {
        delete parent_thread; delete child_a; delete child_b;
        (void)process_detach_child(parent, child);
        (void)process_detach_child(parent, empty_child);
        (void)parent->release(); (void)child->release(); (void)empty_child->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("controlled process termination: FAIL (thread setup)\n");
        return false;
    }
    bool parent_published = scheduler_add_thread(parent_thread) == SchedulerPublishResult::Success;
    bool child_a_published = parent_published &&
        scheduler_add_thread(child_a) == SchedulerPublishResult::Success;
    bool child_b_published = child_a_published &&
        scheduler_add_thread(child_b) == SchedulerPublishResult::Success;
    if (!child_b_published) {
        if (parent_thread->scheduler_enrolled) scheduler_terminate_thread(parent_thread);
        else delete parent_thread;
        if (child_a->scheduler_enrolled) scheduler_terminate_thread(child_a); else delete child_a;
        if (child_b->scheduler_enrolled) scheduler_terminate_thread(child_b); else delete child_b;
        while (scheduler_count_process_threads(parent) || scheduler_count_process_threads(child))
            scheduler_yield();
        (void)process_detach_child(parent, child);
        (void)process_detach_child(parent, empty_child);
        (void)parent->release(); (void)child->release(); (void)empty_child->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("controlled process termination: FAIL (publication)\n");
        return false;
    }

    bool parent_exited = termination_wait_exited(parent);
    bool child_exited = termination_wait_exited(child);
    uint64_t stopped_counter = __atomic_load_n(&shared[14], __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < 100; ++i) scheduler_yield();
    bool all_target_threads_stopped = scheduler_count_process_threads(child) == 0 &&
        scheduler_count_running_process_cpus(child) == 0 &&
        __atomic_load_n(&shared[14], __ATOMIC_ACQUIRE) == stopped_counter;
    bool wait_collected_status = parent->child_count() == 0 && child->parent_pid() == 0 &&
        empty_child->parent_pid() == 0 && shared[17] == 1 &&
        shared[15] == 1 && shared[16] == 0 && child->exit_status() == 91 &&
        child->has_explicit_exit_status();
    bool refs_drained = false;
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        if (parent->reference_count() == 1 && child->reference_count() == 1 &&
            empty_child->reference_count() == 1) {
            refs_drained = true;
            break;
        }
        scheduler_yield();
    }
    bool passed = parent_exited && child_exited && all_target_threads_stopped &&
        wait_collected_status && refs_drained;
    boot_diagnostics_write(all_target_threads_stopped
        ? "controlled termination stopped all child threads: PASS\n"
        : "controlled termination stopped all child threads: FAIL\n");
    boot_diagnostics_write(wait_collected_status
        ? "controlled termination waitpid status and reap: PASS\n"
        : "controlled termination waitpid status and reap: FAIL\n");
    boot_diagnostics_write(passed
        ? "controlled process termination SMP runtime: PASS\n"
        : "controlled process termination SMP runtime: FAIL\n");

    if (parent->state() == ProcessState::Exited) (void)parent->release();
    if (child->state() == ProcessState::Exited) (void)child->release();
    if (empty_child->state() == ProcessState::Exited) (void)empty_child->release();
    pmm_free_page(code_frame); pmm_free_page(data_frame);
    return passed;
}
