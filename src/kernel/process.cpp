#include "kernel/thread.hpp"

#include "memory/vmm.hpp"
#include "memory/heap.hpp"
#include "kernel/panic.hpp"
#include "module.hpp"

namespace {
constexpr uint32_t kDestroyingReferenceCount = UINT32_MAX;
constexpr uintptr_t kInitialUserStackTop = 0x00007FFFF0000000ULL;
static volatile bool s_relationship_lock = false;

uint64_t relationship_lock() {
#ifdef DRAKOS_HOST_TEST
    while (__atomic_test_and_set(&s_relationship_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return 0;
#else
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    while (__atomic_test_and_set(&s_relationship_lock, __ATOMIC_ACQUIRE))
        asm volatile("pause");
    return flags;
#endif
}

void relationship_unlock(uint64_t flags) {
    __atomic_clear(&s_relationship_lock, __ATOMIC_RELEASE);
#ifndef DRAKOS_HOST_TEST
    if (flags & 0x200) asm volatile("sti" ::: "memory");
#else
    (void)flags;
#endif
}

uint64_t allocate_process_id() {
    static uint64_t next_pid = 1;
    uint64_t current = __atomic_load_n(&next_pid, __ATOMIC_RELAXED);
    for (;;) {
        if (!current || current == UINT64_MAX) return 0;
        if (__atomic_compare_exchange_n(&next_pid, &current, current + 1, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return current;
    }
}
}

#ifdef DRAKOS_HOST_TEST
bool scheduler_prepare_wait_current_thread(Process*) { return false; }
void scheduler_clear_current_thread_wait() {}
void scheduler_wake_process_waiters(Process*) {}
void scheduler_yield() {}
ProcessTerminationRequestResult scheduler_terminate_process(Process*, int32_t) {
    return ProcessTerminationRequestResult::InvalidState;
}
#endif

Process::Process()
    : pid(allocate_process_id()), page_table_phys(vmm_get_kernel_pml4()), is_user(false),
      scheduler_threads(0), next_user_stack_top(kInitialUserStackTop),
      scheduler_cleanup_pending(false), scheduler_cleanup_reference_held(false),
      scheduler_next_pending(nullptr),
      handles(), vm_regions(), next_mmap_address(0x100000000ULL),
      owns_address_space(false), references(1),
      lifecycle_state(static_cast<uint8_t>(ProcessState::Ready)), stored_exit_status(0),
      explicit_exit_status(false), relationship_parent(nullptr),
      relationship_first_child(nullptr), relationship_next_sibling(nullptr),
      user_mapping_lock(false) {
    if (!pid) panic("Process ID space exhausted");
}

Process::Process(UserProcessTag)
    : pid(allocate_process_id()), page_table_phys(vmm_get_kernel_pml4()), is_user(false),
      scheduler_threads(0), next_user_stack_top(kInitialUserStackTop),
      scheduler_cleanup_pending(false), scheduler_cleanup_reference_held(false),
      scheduler_next_pending(nullptr),
      handles(), vm_regions(), next_mmap_address(0x100000000ULL),
      owns_address_space(false), references(1),
      lifecycle_state(static_cast<uint8_t>(ProcessState::Created)), stored_exit_status(0),
      explicit_exit_status(false), relationship_parent(nullptr),
      relationship_first_child(nullptr), relationship_next_sibling(nullptr),
      user_mapping_lock(false) {
}

Process::~Process() {
    if (owns_address_space || state() != ProcessState::Exited ||
        scheduler_cleanup_pending || scheduler_cleanup_reference_held || relationship_parent ||
        relationship_first_child || relationship_next_sibling)
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
    if (!process->pid) {
        __atomic_store_n(&process->lifecycle_state,
                         static_cast<uint8_t>(ProcessState::Exited), __ATOMIC_RELEASE);
        (void)process->release();
        return ProcessResult::OutOfMemory;
    }

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

int32_t Process::exit_status() const {
    return __atomic_load_n(&stored_exit_status, __ATOMIC_ACQUIRE);
}

bool Process::has_explicit_exit_status() const {
    return __atomic_load_n(&explicit_exit_status, __ATOMIC_ACQUIRE);
}

uint64_t Process::parent_pid() const {
    uint64_t flags = relationship_lock();
    uint64_t result = relationship_parent ? relationship_parent->pid : 0;
    relationship_unlock(flags);
    return result;
}

bool Process::has_child_pid(uint64_t child_pid) const {
    uint64_t flags = relationship_lock();
    bool found = false;
    for (Process* child = relationship_first_child; child;
         child = child->relationship_next_sibling) {
        if (child->pid == child_pid) { found = true; break; }
    }
    relationship_unlock(flags);
    return found;
}

uint32_t Process::child_count() const {
    uint64_t flags = relationship_lock();
    uint32_t count = 0;
    for (Process* child = relationship_first_child; child;
         child = child->relationship_next_sibling) ++count;
    relationship_unlock(flags);
    return count;
}

void Process::lock_user_mappings() {
    while (__atomic_test_and_set(&user_mapping_lock, __ATOMIC_ACQUIRE))
        asm volatile("pause");
}

void Process::unlock_user_mappings() {
    __atomic_clear(&user_mapping_lock, __ATOMIC_RELEASE);
}

ProcessRelationshipResult process_attach_child(Process* parent, Process* child) {
    if (!parent || !child || parent == child || !parent->is_user || !child->is_user)
        return ProcessRelationshipResult::InvalidProcess;
    uint64_t flags = relationship_lock();
    ProcessState parent_state = parent->state();
    ProcessState child_state = child->state();
    if ((parent_state != ProcessState::Ready && parent_state != ProcessState::Running) ||
        child_state != ProcessState::Ready) {
        relationship_unlock(flags);
        return ProcessRelationshipResult::InvalidProcess;
    }
    for (Process* cursor = parent->relationship_first_child; cursor;
         cursor = cursor->relationship_next_sibling) {
        if (cursor == child || cursor->pid == child->pid) {
            relationship_unlock(flags);
            return ProcessRelationshipResult::DuplicateChild;
        }
    }
    if (child->relationship_parent) {
        relationship_unlock(flags);
        return ProcessRelationshipResult::AlreadyParented;
    }
    if (!child->retain()) {
        relationship_unlock(flags);
        return ProcessRelationshipResult::ReferenceOverflow;
    }
    child->relationship_parent = parent; // weak back-reference; parent owns the link reference
    child->relationship_next_sibling = parent->relationship_first_child;
    parent->relationship_first_child = child;
    relationship_unlock(flags);
    return ProcessRelationshipResult::Success;
}

bool process_detach_child(Process* parent, Process* child) {
    if (!parent || !child) return false;
    uint64_t flags = relationship_lock();
    if (child->relationship_parent != parent) {
        relationship_unlock(flags);
        return false;
    }
    Process** cursor = &parent->relationship_first_child;
    while (*cursor && *cursor != child) cursor = &(*cursor)->relationship_next_sibling;
    if (*cursor != child) {
        relationship_unlock(flags);
        return false;
    }
    *cursor = child->relationship_next_sibling;
    child->relationship_parent = nullptr;
    child->relationship_next_sibling = nullptr;
    relationship_unlock(flags);
    if (child->release() != ProcessResult::Success)
        panic("Detached child relationship reference could not be released");
    return true;
}

ProcessWaitResult process_wait_child(Process* parent, int64_t requested_pid,
                                     uint64_t* child_pid, int32_t* exit_status) {
    if (!parent || !parent->is_user || !child_pid || requested_pid == 0 || requested_pid < -1)
        return requested_pid == 0 || requested_pid < -1
            ? ProcessWaitResult::InvalidPid : ProcessWaitResult::InvalidParent;
    for (;;) {
        uint64_t flags = relationship_lock();
        Process* match = nullptr;
        bool has_eligible_child = false;
        for (Process* child = parent->relationship_first_child; child;
             child = child->relationship_next_sibling) {
            if (requested_pid != -1 && child->pid != static_cast<uint64_t>(requested_pid))
                continue;
            has_eligible_child = true;
            if (child->state() == ProcessState::Exited) { match = child; break; }
        }
        if (match) {
            if (!match->retain()) {
                relationship_unlock(flags);
                return ProcessWaitResult::InvalidParent;
            }
            uint64_t pid = match->pid;
            int32_t status = match->exit_status();
            Process** cursor = &parent->relationship_first_child;
            while (*cursor && *cursor != match)
                cursor = &(*cursor)->relationship_next_sibling;
            if (*cursor != match) {
                relationship_unlock(flags);
                (void)match->release();
                return ProcessWaitResult::NoChild;
            }
            *cursor = match->relationship_next_sibling;
            match->relationship_parent = nullptr;
            match->relationship_next_sibling = nullptr;
            relationship_unlock(flags);
            scheduler_wake_process_waiters(parent);
            if (match->release() != ProcessResult::Success ||
                match->release() != ProcessResult::Success)
                panic("Reaped child relationship references could not be released");
            *child_pid = pid;
            if (exit_status) *exit_status = status;
            return ProcessWaitResult::Success;
        }
        if (!has_eligible_child) {
            relationship_unlock(flags);
            return ProcessWaitResult::NoChild;
        }
        if (!scheduler_prepare_wait_current_thread(parent)) {
            relationship_unlock(flags);
            return ProcessWaitResult::SchedulerUnavailable;
        }
        relationship_unlock(flags);
        scheduler_yield();
        scheduler_clear_current_thread_wait();
    }
}

ProcessTerminationRequestResult process_terminate_child(Process* requester,
                                                        uint64_t target_pid,
                                                        int32_t status) {
    if (!target_pid) return ProcessTerminationRequestResult::InvalidPid;
    if (!requester || !requester->is_user)
        return ProcessTerminationRequestResult::InvalidProcess;

    uint64_t flags = relationship_lock();
    ProcessState requester_state = requester->state();
    if (requester_state != ProcessState::Ready && requester_state != ProcessState::Running) {
        relationship_unlock(flags);
        return ProcessTerminationRequestResult::InvalidState;
    }
    Process* target = requester->relationship_first_child;
    while (target && target->pid != target_pid) target = target->relationship_next_sibling;
    if (!target) {
        relationship_unlock(flags);
        return ProcessTerminationRequestResult::NoSuchProcess;
    }
    if (!target->retain()) {
        relationship_unlock(flags);
        return ProcessTerminationRequestResult::InvalidProcess;
    }
    relationship_unlock(flags);

    ProcessTerminationRequestResult result = scheduler_terminate_process(target, status);
    if (target->release() != ProcessResult::Success)
        panic("Controlled termination target reference could not be released");
    return result;
}

void process_finish_relationships(Process* process) {
    if (!process) return;
    Process* parent_to_wake = nullptr;
    Process* orphaned = nullptr;
    uint64_t flags = relationship_lock();
    if (process->relationship_parent) {
        parent_to_wake = process->relationship_parent;
        if (!parent_to_wake->retain()) parent_to_wake = nullptr;
    }
    orphaned = process->relationship_first_child;
    process->relationship_first_child = nullptr;
    for (Process* child = orphaned; child; child = child->relationship_next_sibling)
        child->relationship_parent = nullptr;
    relationship_unlock(flags);

    if (parent_to_wake) {
        scheduler_wake_process_waiters(parent_to_wake);
        if (parent_to_wake->release() != ProcessResult::Success)
            panic("Parent notification reference could not be released");
    }
    while (orphaned) {
        Process* child = orphaned;
        orphaned = child->relationship_next_sibling;
        child->relationship_next_sibling = nullptr;
        if (child->release() != ProcessResult::Success)
            panic("Orphaned child relationship reference could not be released");
    }
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
    process_finish_relationships(this);
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
