#include "kernel/thread.hpp"
#include "memory/pmm.hpp"
#include "memory/vmm.hpp"

#include <cstdio>

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
        ok &= vmm_retain_address_space(terminal_root) == VmmAddressSpaceResult::InvalidAddressSpace;
        ok &= terminal->release() == ProcessResult::Success;
        ok &= terminal->state() == ProcessState::Exited;
        ok &= terminal->release() == ProcessResult::Success;
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
