#include "kernel/thread.hpp"

#include "memory/vmm.hpp"
#include "memory/heap.hpp"
#include "kernel/panic.hpp"
#include "module.hpp"

namespace {
constexpr uint32_t kDestroyingReferenceCount = UINT32_MAX;
constexpr uintptr_t kInitialUserStackTop = 0x00007FFFF0000000ULL;

uint64_t allocate_process_id() {
    static uint64_t next_pid = 1;
    return __atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED);
}
}

Process::Process()
    : pid(allocate_process_id()), page_table_phys(vmm_get_kernel_pml4()), is_user(false),
      scheduler_threads(0), next_user_stack_top(kInitialUserStackTop),
      scheduler_cleanup_pending(false), scheduler_next_pending(nullptr),
      handles(), vm_regions(), next_mmap_address(0x100000000ULL),
      owns_address_space(false), references(1),
      lifecycle_state(static_cast<uint8_t>(ProcessState::Ready)) {
}

Process::Process(UserProcessTag)
    : pid(allocate_process_id()), page_table_phys(vmm_get_kernel_pml4()), is_user(false),
      scheduler_threads(0), next_user_stack_top(kInitialUserStackTop),
      scheduler_cleanup_pending(false), scheduler_next_pending(nullptr),
      handles(), vm_regions(), next_mmap_address(0x100000000ULL),
      owns_address_space(false), references(1),
      lifecycle_state(static_cast<uint8_t>(ProcessState::Created)) {
}

Process::~Process() {
    if (owns_address_space || state() != ProcessState::Exited)
        panic("Process released before reaching a resource-free terminal state");
}

ProcessResult Process::create_user(Process** output) {
    if (!output) return ProcessResult::InvalidArgument;
    *output = nullptr;

    Process* process = nullptr;
#ifdef DRAKOS_HOST_TEST
    process = new Process(UserProcessTag{});
#else
    void* storage = kmalloc(sizeof(Process));
    if (storage) process = new (storage) Process(UserProcessTag{});
#endif
    if (!process) return ProcessResult::OutOfMemory;

    if (!process->transition_to(ProcessState::Initializing)) {
        __atomic_store_n(&process->lifecycle_state,
                         static_cast<uint8_t>(ProcessState::Exited), __ATOMIC_RELEASE);
        (void)process->release();
        return ProcessResult::InvalidState;
    }

    uintptr_t root = vmm_create_address_space();
    if (!root) {
        __atomic_store_n(&process->lifecycle_state,
                         static_cast<uint8_t>(ProcessState::Exited), __ATOMIC_RELEASE);
        (void)process->release();
        return ProcessResult::OutOfMemory;
    }

    process->page_table_phys = root;
    process->owns_address_space = true;
    process->is_user = true;
    if (!process->transition_to(ProcessState::Ready)) {
        VmmAddressSpaceResult destroyed = vmm_destroy_address_space(root);
        if (destroyed != VmmAddressSpaceResult::Success) {
            panic("Process creation rollback could not destroy its address space");
        }
        process->page_table_phys = vmm_get_kernel_pml4();
        process->owns_address_space = false;
        process->is_user = false;
        __atomic_store_n(&process->lifecycle_state,
                         static_cast<uint8_t>(ProcessState::Exited), __ATOMIC_RELEASE);
        (void)process->release();
        return ProcessResult::InvalidState;
    }

    *output = process;
    return ProcessResult::Success;
}

bool Process::valid_transition(ProcessState from, ProcessState to) {
    switch (from) {
        case ProcessState::Created:
            return to == ProcessState::Initializing;
        case ProcessState::Initializing:
            return to == ProcessState::Ready;
        case ProcessState::Ready:
            return to == ProcessState::Running || to == ProcessState::Terminating;
        case ProcessState::Running:
            return to == ProcessState::Terminating;
        case ProcessState::Terminating:
            return to == ProcessState::Exited;
        case ProcessState::Exited:
            return false;
    }
    return false;
}

ProcessState Process::state() const {
    return static_cast<ProcessState>(__atomic_load_n(&lifecycle_state, __ATOMIC_ACQUIRE));
}

bool Process::transition_to(ProcessState next) {
    if (next == ProcessState::Exited) return false;
    uint8_t current = __atomic_load_n(&lifecycle_state, __ATOMIC_ACQUIRE);
    for (;;) {
        ProcessState from = static_cast<ProcessState>(current);
        if (from == next) return true;
        if (!valid_transition(from, next)) return false;
        uint8_t desired = static_cast<uint8_t>(next);
        if (__atomic_compare_exchange_n(&lifecycle_state, &current, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
}

bool Process::retain() {
    uint32_t current = __atomic_load_n(&references, __ATOMIC_ACQUIRE);
    while (current && current != kDestroyingReferenceCount) {
        if (current == kDestroyingReferenceCount - 1) return false;
        if (__atomic_compare_exchange_n(&references, &current, current + 1, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

uint32_t Process::reference_count() const {
    return __atomic_load_n(&references, __ATOMIC_ACQUIRE);
}

ProcessResult Process::destroy_resources() {
    if (scheduler_threads != 0) return ProcessResult::InUse;
    if (owns_address_space) {
        VmmAddressSpaceResult result = vmm_destroy_address_space(page_table_phys);
        if (result != VmmAddressSpaceResult::Success)
            return result == VmmAddressSpaceResult::AddressSpaceInUse ||
                   result == VmmAddressSpaceResult::ActiveAddressSpace ||
                   result == VmmAddressSpaceResult::Busy
                ? ProcessResult::InUse : ProcessResult::CleanupFailure;
        page_table_phys = vmm_get_kernel_pml4();
        owns_address_space = false;
    }
    handles.close_all();
    vm_regions.clear();
    return ProcessResult::Success;
}

ProcessResult Process::finalize_termination() {
    if (state() != ProcessState::Terminating) return ProcessResult::InvalidState;
    ProcessResult resources = destroy_resources();
    if (resources != ProcessResult::Success) return resources;
    uint8_t expected = static_cast<uint8_t>(ProcessState::Terminating);
    if (!__atomic_compare_exchange_n(&lifecycle_state, &expected,
                                     static_cast<uint8_t>(ProcessState::Exited), false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return ProcessResult::InvalidState;
    return ProcessResult::Success;
}

ProcessResult Process::release() {
    uint32_t current = __atomic_load_n(&references, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!current || current == kDestroyingReferenceCount) return ProcessResult::InvalidState;
        if (current > 1) {
            if (__atomic_compare_exchange_n(&references, &current, current - 1, false,
                                           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return ProcessResult::Success;
            continue;
        }
        if (!__atomic_compare_exchange_n(&references, &current, kDestroyingReferenceCount, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;

        ProcessState current_state = state();
        ProcessResult result = ProcessResult::Success;
        if (current_state == ProcessState::Ready) {
            if (!transition_to(ProcessState::Terminating)) result = ProcessResult::InvalidState;
            else result = finalize_termination();
        } else if (current_state == ProcessState::Terminating) {
            result = finalize_termination();
        } else if (current_state != ProcessState::Exited) {
            result = ProcessResult::InvalidState;
        }

        if (result != ProcessResult::Success) {
            __atomic_store_n(&references, 1, __ATOMIC_RELEASE);
            return result;
        }
        __atomic_store_n(&references, 0, __ATOMIC_RELEASE);
        delete this;
        return ProcessResult::Success;
    }
}
