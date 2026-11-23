#include "kernel/thread.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"

#include <cstdio>

struct ProcessLifecycleTestAccess {
    static void set_exit_status(Process* process, int32_t status) {
        __atomic_store_n(&process->stored_exit_status, status, __ATOMIC_RELEASE);
        __atomic_store_n(&process->explicit_exit_status, true, __ATOMIC_RELEASE);
    }
};

static size_t count_available_pages() {
    uintptr_t pages[4096];
    size_t count = 0;
    while (count < sizeof(pages) / sizeof(pages[0])) {
        uintptr_t page = pmm_alloc_page();
        if (!page) break;
        pages[count++] = page;
    }
    for (size_t i = count; i > 0; --i) pmm_free_page(pages[i - 1]);
    return count;
}

bool run_process_lifecycle_test() {
    bool ok = true;
    Process* process = nullptr;
    ProcessResult created = Process::create_user(&process);
    ok &= created == ProcessResult::Success && process;
    if (!process) return false;

    uintptr_t root = process->page_table_phys;
    uint64_t process_id = process->pid;
    ok &= process->state() == ProcessState::Ready;
    ok &= process->exit_status() == 0 && !process->has_explicit_exit_status();
    ok &= process->exit_status() == 0 && !process->has_explicit_exit_status();
    ok &= process->reference_count() == 1;
    ok &= process->scheduler_threads == 0;
    ok &= vmm_retain_address_space(root) == VmmAddressSpaceResult::Success;
    ok &= vmm_release_address_space(root) == VmmAddressSpaceResult::Success;

    ok &= process->retain();
    ok &= process->reference_count() == 2;
    ok &= process->release() == ProcessResult::Success;
    ok &= process->reference_count() == 1;
    ok &= process->state() == ProcessState::Ready;
    ok &= process->release() == ProcessResult::Success;
    ok &= vmm_retain_address_space(root) == VmmAddressSpaceResult::InvalidAddressSpace;

    Process* terminal = nullptr;
    ok &= Process::create_user(&terminal) == ProcessResult::Success && terminal;
    if (terminal) {
        ok &= terminal->pid != process_id;
        uintptr_t terminal_root = terminal->page_table_phys;
        ok &= !terminal->transition_to(ProcessState::Initializing);
        ok &= terminal->transition_to(ProcessState::Running);
        ok &= !terminal->transition_to(ProcessState::Ready);
        ok &= terminal->retain();
        ok &= terminal->transition_to(ProcessState::Terminating);
        ok &= !terminal->transition_to(ProcessState::Running);
        ok &= terminal->finalize_termination() == ProcessResult::Success;
        ok &= terminal->state() == ProcessState::Exited;
        ok &= terminal->exit_status() == 0 && !terminal->has_explicit_exit_status();
        ok &= terminal->exit_status() == 0 && !terminal->has_explicit_exit_status();
        ok &= vmm_retain_address_space(terminal_root) == VmmAddressSpaceResult::InvalidAddressSpace;
        ok &= terminal->release() == ProcessResult::Success;
        ok &= terminal->state() == ProcessState::Exited;
        ok &= terminal->release() == ProcessResult::Success;
    }

    Process* parent = nullptr;
    Process* child = nullptr;
    Process* other_parent = nullptr;
    ok &= Process::create_user(&parent) == ProcessResult::Success && parent;
    ok &= Process::create_user(&child) == ProcessResult::Success && child;
    ok &= Process::create_user(&other_parent) == ProcessResult::Success && other_parent;
    if (parent && child && other_parent) {
        ok &= process_attach_child(parent, parent) == ProcessRelationshipResult::InvalidProcess;
        ok &= process_attach_child(parent, child) == ProcessRelationshipResult::Success;
        ok &= child->parent_pid() == parent->pid && parent->has_child_pid(child->pid) &&
            parent->child_count() == 1 && child->reference_count() == 2;
        ok &= process_attach_child(parent, child) == ProcessRelationshipResult::DuplicateChild;
        ok &= process_attach_child(other_parent, child) == ProcessRelationshipResult::AlreadyParented;
        ok &= process_terminate_child(parent, 0, 99) ==
            ProcessTerminationRequestResult::InvalidPid;
        ok &= process_terminate_child(parent, other_parent->pid, 99) ==
            ProcessTerminationRequestResult::NoSuchProcess;
        ok &= process_terminate_child(parent, child->pid, 99) ==
            ProcessTerminationRequestResult::InvalidState;

        uint64_t child_pid = 0;
        int32_t status = -1;
        ok &= process_wait_child(parent, static_cast<int64_t>(child->pid),
            &child_pid, &status) == ProcessWaitResult::SchedulerUnavailable;

        ok &= child->transition_to(ProcessState::Running);
        ok &= child->transition_to(ProcessState::Terminating);
        ProcessLifecycleTestAccess::set_exit_status(child, 23);
        ok &= child->finalize_termination() == ProcessResult::Success;
        ok &= child->parent_pid() == parent->pid && parent->has_child_pid(child->pid);
        ok &= process_wait_child(parent, static_cast<int64_t>(child->pid),
            &child_pid, &status) == ProcessWaitResult::Success;
        ok &= child_pid == child->pid && status == 23 && child->parent_pid() == 0 &&
            !parent->has_child_pid(child->pid) && parent->child_count() == 0 &&
            child->reference_count() == 1;
        ok &= process_wait_child(parent, static_cast<int64_t>(child->pid),
            &child_pid, &status) == ProcessWaitResult::NoChild;
        ok &= process_wait_child(parent, 0, &child_pid, &status) == ProcessWaitResult::InvalidPid;
        ok &= process_wait_child(parent, -2, &child_pid, &status) == ProcessWaitResult::InvalidPid;
        ok &= process_wait_child(parent, -1, &child_pid, &status) == ProcessWaitResult::NoChild;
        ok &= child->release() == ProcessResult::Success;

        Process* orphan = nullptr;
        ok &= Process::create_user(&orphan) == ProcessResult::Success && orphan;
        if (orphan) {
            ok &= process_attach_child(parent, orphan) == ProcessRelationshipResult::Success;
            ok &= parent->transition_to(ProcessState::Running);
            ok &= parent->transition_to(ProcessState::Terminating);
            ok &= parent->finalize_termination() == ProcessResult::Success;
            ok &= orphan->parent_pid() == 0 && parent->child_count() == 0 &&
                orphan->reference_count() == 1;
            ok &= parent->release() == ProcessResult::Success;
            ok &= orphan->release() == ProcessResult::Success;
        }
        ok &= other_parent->release() == ProcessResult::Success;
    } else {
        if (parent) (void)parent->release();
        if (child) (void)child->release();
        if (other_parent) (void)other_parent->release();
        ok = false;
    }

    size_t pages_before_failure = count_available_pages();
    vmm_test_fail_registry_allocation_after(0);
    Process* failed = nullptr;
    ProcessResult failure = Process::create_user(&failed);
    size_t pages_after_failure = count_available_pages();
    ok &= failure == ProcessResult::OutOfMemory && failed == nullptr;
    ok &= pages_before_failure == pages_after_failure;

    if (!ok) std::fprintf(stderr, "FAIL: process lifecycle and creation transaction checks\n");
    return ok;
}
