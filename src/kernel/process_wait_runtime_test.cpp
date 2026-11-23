#include "kernel/address_space_runtime_test.hpp"

#include "kernel/boot_diagnostics.hpp"
#include "kernel/thread.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"
#include "cpu.hpp"

extern "C" const uint8_t process_wait_blob_start[];
extern "C" const uint8_t process_wait_blob_end[];
extern "C" const uint8_t process_wait_blob_final_end[];
extern "C" const uint8_t process_wait_specific[];
extern "C" const uint8_t process_wait_any[];
extern "C" const uint8_t process_wait_child_worker[];
extern "C" const uint8_t process_wait_multi_parent[];
extern "C" const uint8_t process_wait_child_worker2[];
extern "C" const uint8_t process_wait_orphan_parent[];

namespace {
constexpr uintptr_t kWaitCodeVa = 0x00400000;
constexpr uintptr_t kWaitDataVa = 0x00401000;
void clear_frame(uintptr_t frame) {
    auto* bytes = reinterpret_cast<uint8_t*>(frame + pmm_hhdm_offset());
    for (size_t i = 0; i < PAGE_SIZE; ++i) bytes[i] = 0;
}

bool map_wait_pages(Process* process, uintptr_t code, uintptr_t data) {
    uint64_t data_flags = VMM_PRESENT | VMM_USER | VMM_WRITE;
    if (nx_enabled()) data_flags |= VMM_NX;
    return vmm_map_page(process->page_table_phys, kWaitCodeVa, code,
                        VMM_PRESENT | VMM_USER) == VmmMapResult::Success &&
           vmm_map_page(process->page_table_phys, kWaitDataVa, data,
                        data_flags) == VmmMapResult::Success;
}

bool wait_until_exited(Process* process) {
    for (size_t attempt = 0; attempt < 10000 &&
         process->state() != ProcessState::Exited; ++attempt) scheduler_yield();
    return process->state() == ProcessState::Exited;
}
}

bool run_process_wait_runtime_test() {
    uintptr_t code_frame = pmm_alloc_page();
    uintptr_t shared_data_frame = pmm_alloc_page();
    if (!code_frame || !shared_data_frame) {
        if (code_frame) pmm_free_page(code_frame);
        if (shared_data_frame) pmm_free_page(shared_data_frame);
        boot_diagnostics_write("process wait/reap: FAIL (page allocation)\n");
        return false;
    }
    clear_frame(code_frame);
    clear_frame(shared_data_frame);
    auto* code = reinterpret_cast<uint8_t*>(code_frame + pmm_hhdm_offset());
    size_t blob_size = static_cast<size_t>(process_wait_blob_final_end - process_wait_blob_start);
    for (size_t i = 0; i < blob_size; ++i) code[i] = process_wait_blob_start[i];

    Process* parent = nullptr;
    Process* child = nullptr;
    bool processes_ready = Process::create_user(&parent) == ProcessResult::Success && parent &&
        Process::create_user(&child) == ProcessResult::Success && child;
    bool mapped = processes_ready && map_wait_pages(parent, code_frame, shared_data_frame) &&
        map_wait_pages(child, code_frame, shared_data_frame);
    if (!mapped || process_attach_child(parent, child) != ProcessRelationshipResult::Success) {
        if (parent) (void)parent->release();
        if (child) (void)child->release();
        pmm_free_page(code_frame);
        pmm_free_page(shared_data_frame);
        boot_diagnostics_write("process wait/reap: FAIL (process setup)\n");
        return false;
    }

    auto* shared = reinterpret_cast<volatile uint64_t*>(
        shared_data_frame + pmm_hhdm_offset());
    shared[0] = child->pid;
    Thread* specific = new Thread(parent,
        reinterpret_cast<void(*)()>(kWaitCodeVa +
            static_cast<uintptr_t>(process_wait_specific - process_wait_blob_start)),
        true, "wait-specific");
    Thread* any = new Thread(parent,
        reinterpret_cast<void(*)()>(kWaitCodeVa +
            static_cast<uintptr_t>(process_wait_any - process_wait_blob_start)),
        true, "wait-any");
    Thread* child_thread = new Thread(child,
        reinterpret_cast<void(*)()>(kWaitCodeVa +
            static_cast<uintptr_t>(process_wait_child_worker - process_wait_blob_start)),
        true, "wait-child");
    if (!specific || !any || !child_thread || specific->state == THREAD_DEAD ||
        any->state == THREAD_DEAD || child_thread->state == THREAD_DEAD) {
        delete specific; delete any; delete child_thread;
        (void)process_detach_child(parent, child);
        (void)parent->release(); (void)child->release();
        pmm_free_page(code_frame); pmm_free_page(shared_data_frame);
        boot_diagnostics_write("process wait/reap: FAIL (thread setup)\n");
        return false;
    }
    bool published = scheduler_add_thread(specific) == SchedulerPublishResult::Success &&
        scheduler_add_thread(any) == SchedulerPublishResult::Success &&
        scheduler_add_thread(child_thread) == SchedulerPublishResult::Success;
    if (!published) {
        if (specific->scheduler_enrolled) scheduler_terminate_thread(specific); else delete specific;
        if (any->scheduler_enrolled) scheduler_terminate_thread(any); else delete any;
        if (child_thread->scheduler_enrolled) scheduler_terminate_thread(child_thread);
        else delete child_thread;
        while (scheduler_count_process_threads(parent) || scheduler_count_process_threads(child))
            scheduler_yield();
        (void)process_detach_child(parent, child);
        (void)parent->release(); (void)child->release();
        pmm_free_page(code_frame); pmm_free_page(shared_data_frame);
        boot_diagnostics_write("process wait/reap: FAIL (thread publication)\n");
        return false;
    }

    bool both_blocked = false;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        if (scheduler_count_process_waiters(parent) == 2) { both_blocked = true; break; }
        scheduler_yield();
    }
    __atomic_store_n(&shared[2], 1, __ATOMIC_RELEASE); // Let the child exit.
    bool wait_results = false;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        if (__atomic_load_n(&shared[3], __ATOMIC_ACQUIRE) == 1 &&
            __atomic_load_n(&shared[4], __ATOMIC_ACQUIRE) == 1) {
            wait_results = true;
            break;
        }
        scheduler_yield();
    }
    bool status_ok = static_cast<int32_t>(shared[1]) == 23;
    bool reaped_once = parent->child_count() == 0 && child->parent_pid() == 0;
    __atomic_store_n(&shared[6], 1, __ATOMIC_RELEASE); // Allow both parent threads to exit.
    bool parent_exited = wait_until_exited(parent);
    bool child_exited = wait_until_exited(child);
    bool refs_drained = false;
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        if (parent->reference_count() == 1 && child->reference_count() == 1) {
            refs_drained = true;
            break;
        }
        scheduler_yield();
    }

    bool passed = both_blocked && wait_results && status_ok && reaped_once &&
        shared[5] == 0 && shared[14] == 0 && parent_exited && child_exited && refs_drained &&
        parent->exit_status() == 0 && child->exit_status() == 23;
    boot_diagnostics_write(both_blocked
        ? "process wait blocked parent threads: PASS\n"
        : "process wait blocked parent threads: FAIL\n");
    boot_diagnostics_write(wait_results && status_ok && reaped_once
        ? "process wait concurrent single reap and status: PASS\n"
        : "process wait concurrent single reap and status: FAIL\n");
    boot_diagnostics_write(passed
        ? "process wait/reap SMP runtime: PASS\n"
        : "process wait/reap SMP runtime: FAIL\n");

    if (parent->state() == ProcessState::Exited) (void)parent->release();
    if (child->state() == ProcessState::Exited) (void)child->release();
    pmm_free_page(code_frame);
    pmm_free_page(shared_data_frame);
    return passed;
}

bool run_process_wait_multi_child_runtime_test() {
    uintptr_t code_frame = pmm_alloc_page();
    uintptr_t data_frame = pmm_alloc_page();
    if (!code_frame || !data_frame) {
        if (code_frame) pmm_free_page(code_frame);
        if (data_frame) pmm_free_page(data_frame);
        return false;
    }
    clear_frame(code_frame);
    clear_frame(data_frame);
    auto* code = reinterpret_cast<uint8_t*>(code_frame + pmm_hhdm_offset());
    size_t blob_size = static_cast<size_t>(process_wait_blob_final_end - process_wait_blob_start);
    for (size_t i = 0; i < blob_size; ++i) code[i] = process_wait_blob_start[i];

    Process* parent = nullptr;
    Process* child_a = nullptr;
    Process* child_b = nullptr;
    bool ready = Process::create_user(&parent) == ProcessResult::Success && parent &&
        Process::create_user(&child_a) == ProcessResult::Success && child_a &&
        Process::create_user(&child_b) == ProcessResult::Success && child_b;
    bool mapped = ready && map_wait_pages(parent, code_frame, data_frame) &&
        map_wait_pages(child_a, code_frame, data_frame) &&
        map_wait_pages(child_b, code_frame, data_frame);
    bool linked_a = mapped &&
        process_attach_child(parent, child_a) == ProcessRelationshipResult::Success;
    bool linked_b = linked_a &&
        process_attach_child(parent, child_b) == ProcessRelationshipResult::Success;
    if (!linked_b) {
        if (linked_a) (void)process_detach_child(parent, child_a);
        if (parent) (void)parent->release();
        if (child_a) (void)child_a->release();
        if (child_b) (void)child_b->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("process wait concurrent children: FAIL (setup)\n");
        return false;
    }

    auto* shared = reinterpret_cast<volatile uint64_t*>(data_frame + pmm_hhdm_offset());
    shared[0] = child_a->pid;
    shared[7] = child_b->pid;
    auto entry = [](const uint8_t* symbol) {
        return reinterpret_cast<void(*)()>(kWaitCodeVa +
            static_cast<uintptr_t>(symbol - process_wait_blob_start));
    };
    Thread* parent_thread = new Thread(parent, entry(process_wait_multi_parent), true,
                                        "wait-any-multiple-children");
    Thread* child_thread_a = new Thread(child_a, entry(process_wait_child_worker), true,
                                         "wait-child-a");
    Thread* child_thread_b = new Thread(child_b, entry(process_wait_child_worker2), true,
                                         "wait-child-b");
    if (!parent_thread || !child_thread_a || !child_thread_b ||
        parent_thread->state == THREAD_DEAD || child_thread_a->state == THREAD_DEAD ||
        child_thread_b->state == THREAD_DEAD) {
        delete parent_thread; delete child_thread_a; delete child_thread_b;
        (void)process_detach_child(parent, child_a);
        (void)process_detach_child(parent, child_b);
        (void)parent->release(); (void)child_a->release(); (void)child_b->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("process wait concurrent children: FAIL (thread setup)\n");
        return false;
    }
    bool pub_parent = scheduler_add_thread(parent_thread) == SchedulerPublishResult::Success;
    bool pub_a = pub_parent &&
        scheduler_add_thread(child_thread_a) == SchedulerPublishResult::Success;
    bool pub_b = pub_a &&
        scheduler_add_thread(child_thread_b) == SchedulerPublishResult::Success;
    if (!pub_b) {
        if (parent_thread->scheduler_enrolled) scheduler_terminate_thread(parent_thread);
        else delete parent_thread;
        if (child_thread_a->scheduler_enrolled) scheduler_terminate_thread(child_thread_a);
        else delete child_thread_a;
        if (child_thread_b->scheduler_enrolled) scheduler_terminate_thread(child_thread_b);
        else delete child_thread_b;
        while (scheduler_count_process_threads(parent) ||
               scheduler_count_process_threads(child_a) ||
               scheduler_count_process_threads(child_b)) scheduler_yield();
        (void)process_detach_child(parent, child_a);
        (void)process_detach_child(parent, child_b);
        (void)parent->release(); (void)child_a->release(); (void)child_b->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("process wait concurrent children: FAIL (publication)\n");
        return false;
    }

    bool blocked = false;
    for (size_t attempt = 0; attempt < 5000; ++attempt) {
        if (scheduler_count_process_waiters(parent) == 1 &&
            __atomic_load_n(&shared[12], __ATOMIC_ACQUIRE) == 1 &&
            __atomic_load_n(&shared[13], __ATOMIC_ACQUIRE) == 1) {
            blocked = true;
            break;
        }
        scheduler_yield();
    }
    // Both child threads observe this release and race through process exit.
    __atomic_store_n(&shared[2], 1, __ATOMIC_RELEASE);
    bool both_waited = false;
    for (size_t attempt = 0; attempt < 8000; ++attempt) {
        if (__atomic_load_n(&shared[3], __ATOMIC_ACQUIRE) == 2 &&
            child_a->state() == ProcessState::Exited &&
            child_b->state() == ProcessState::Exited) {
            both_waited = true;
            break;
        }
        scheduler_yield();
    }
    bool statuses_and_pids =
        ((shared[10] == child_a->pid && static_cast<int32_t>(shared[1]) == 23 &&
          shared[11] == child_b->pid && static_cast<int32_t>(shared[8]) == 47) ||
         (shared[10] == child_b->pid && static_cast<int32_t>(shared[1]) == 47 &&
          shared[11] == child_a->pid && static_cast<int32_t>(shared[8]) == 23));
    bool both_reaped = parent->child_count() == 0 && child_a->parent_pid() == 0 &&
        child_b->parent_pid() == 0;
    __atomic_store_n(&shared[6], 1, __ATOMIC_RELEASE);
    bool parent_exited = wait_until_exited(parent);
    bool refs_drained = false;
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        if (parent->reference_count() == 1 && child_a->reference_count() == 1 &&
            child_b->reference_count() == 1) { refs_drained = true; break; }
        scheduler_yield();
    }
    bool passed = blocked && both_waited && statuses_and_pids && both_reaped &&
        shared[5] == 0 && parent_exited && refs_drained;
    boot_diagnostics_write(blocked ? "wait-any parent blocked with both children ready: PASS\n"
                                   : "wait-any parent blocked with both children ready: FAIL\n");
    boot_diagnostics_write(both_waited ? "wait-any collected two child results: PASS\n"
                                       : "wait-any collected two child results: FAIL\n");
    boot_diagnostics_write(statuses_and_pids ? "wait-any PID/status association: PASS\n"
                                             : "wait-any PID/status association: FAIL\n");
    boot_diagnostics_write(both_reaped ? "wait-any removed both relationships: PASS\n"
                                        : "wait-any removed both relationships: FAIL\n");
    boot_diagnostics_write(shared[5] == 0 ? "wait-any no internal errors: PASS\n"
                                          : "wait-any no internal errors: FAIL\n");
    boot_diagnostics_write(parent_exited && refs_drained
        ? "wait-any cleanup references drained: PASS\n"
        : "wait-any cleanup references drained: FAIL\n");
    boot_diagnostics_write(passed
        ? "process wait-any concurrent child exits and reap: PASS\n"
        : "process wait-any concurrent child exits and reap: FAIL\n");
    if (parent->state() == ProcessState::Exited) (void)parent->release();
    if (child_a->state() == ProcessState::Exited) (void)child_a->release();
    if (child_b->state() == ProcessState::Exited) (void)child_b->release();
    pmm_free_page(code_frame); pmm_free_page(data_frame);
    return passed;
}

bool run_process_orphan_runtime_test() {
    uintptr_t code_frame = pmm_alloc_page();
    uintptr_t data_frame = pmm_alloc_page();
    if (!code_frame || !data_frame) {
        if (code_frame) pmm_free_page(code_frame);
        if (data_frame) pmm_free_page(data_frame);
        return false;
    }
    clear_frame(code_frame);
    clear_frame(data_frame);
    auto* code = reinterpret_cast<uint8_t*>(code_frame + pmm_hhdm_offset());
    size_t blob_size = static_cast<size_t>(process_wait_blob_final_end - process_wait_blob_start);
    for (size_t i = 0; i < blob_size; ++i) code[i] = process_wait_blob_start[i];

    Process* parent = nullptr;
    Process* child = nullptr;
    bool ready = Process::create_user(&parent) == ProcessResult::Success && parent &&
        Process::create_user(&child) == ProcessResult::Success && child;
    bool linked = ready && map_wait_pages(parent, code_frame, data_frame) &&
        map_wait_pages(child, code_frame, data_frame) &&
        process_attach_child(parent, child) == ProcessRelationshipResult::Success;
    if (!linked) {
        if (parent) (void)parent->release();
        if (child) (void)child->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("process orphan active child: FAIL (setup)\n");
        return false;
    }

    auto entry = [](const uint8_t* symbol) {
        return reinterpret_cast<void(*)()>(kWaitCodeVa +
            static_cast<uintptr_t>(symbol - process_wait_blob_start));
    };
    Thread* parent_thread = new Thread(parent, entry(process_wait_orphan_parent), true,
                                        "orphan-parent-exit");
    Thread* child_thread = new Thread(child, entry(process_wait_child_worker2), true,
                                       "orphan-child-continues");
    if (!parent_thread || !child_thread || parent_thread->state == THREAD_DEAD ||
        child_thread->state == THREAD_DEAD) {
        delete parent_thread; delete child_thread;
        (void)process_detach_child(parent, child);
        (void)parent->release(); (void)child->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("process orphan active child: FAIL (thread setup)\n");
        return false;
    }
    bool parent_published = scheduler_add_thread(parent_thread) == SchedulerPublishResult::Success;
    bool child_published = parent_published &&
        scheduler_add_thread(child_thread) == SchedulerPublishResult::Success;
    if (!child_published) {
        if (parent_thread->scheduler_enrolled) scheduler_terminate_thread(parent_thread);
        else delete parent_thread;
        if (child_thread->scheduler_enrolled) scheduler_terminate_thread(child_thread);
        else delete child_thread;
        while (scheduler_count_process_threads(parent) || scheduler_count_process_threads(child))
            scheduler_yield();
        (void)process_detach_child(parent, child);
        (void)parent->release(); (void)child->release();
        pmm_free_page(code_frame); pmm_free_page(data_frame);
        boot_diagnostics_write("process orphan active child: FAIL (publication)\n");
        return false;
    }

    auto* shared = reinterpret_cast<volatile uint64_t*>(data_frame + pmm_hhdm_offset());
    bool parent_exited = wait_until_exited(parent);
    bool orphan_continued = parent_exited && parent->child_count() == 0 &&
        child->parent_pid() == 0 && child->state() != ProcessState::Exited &&
        scheduler_count_process_threads(child) == 1 && shared[13] == 1;
    __atomic_store_n(&shared[2], 1, __ATOMIC_RELEASE);
    bool child_exited = wait_until_exited(child);
    bool refs_drained = false;
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        if (parent->reference_count() == 1 && child->reference_count() == 1) {
            refs_drained = true;
            break;
        }
        scheduler_yield();
    }
    bool passed = orphan_continued && child_exited && refs_drained &&
        child->exit_status() == 47;
    boot_diagnostics_write(passed
        ? "parent exit orphaned live child safely: PASS\n"
        : "parent exit orphaned live child safely: FAIL\n");
    if (parent->state() == ProcessState::Exited) (void)parent->release();
    if (child->state() == ProcessState::Exited) (void)child->release();
    pmm_free_page(code_frame); pmm_free_page(data_frame);
    return passed;
}
