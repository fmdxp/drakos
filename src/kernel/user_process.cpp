#include "kernel/user_process.hpp"

#include "kernel/panic.hpp"
#include "memory/vmm.hpp"

namespace {
static volatile bool s_fail_next_thread_allocation = false;

UserProcessCreateResult create_result(UserProcessCreateError error) {
    return {error, ProcessResult::Success,
            {ElfLoadError::Success, ElfLoadError::Success,
             VmmMapResult::Success, VmRegionResult::Success},
            ThreadInitResult::Success, VmmMapResult::Success,
            VmRegionResult::Success, SchedulerPublishResult::Success};
}

void cleanup_unpublished(Process* process, Thread* thread) {
    if (thread) delete thread;
    if (process && process->release() != ProcessResult::Success)
        panic("Unable to roll back an unpublished user process");
}

bool valid_initial_context(Process* process, const Thread* thread,
                           uintptr_t entry_point) {
    constexpr uintptr_t USER_LIMIT = 1ULL << 47;
    if (!process || !thread || thread->parent_process != process || !thread->context ||
        !process->is_user || thread->context->rip != entry_point ||
        thread->context->cs != 0x23 || thread->context->ss != 0x1B ||
        !(thread->context->rflags & 0x200) || (thread->context->rflags & (3ULL << 12)) ||
        !thread->context->rsp || thread->context->rsp >= USER_LIMIT ||
        (thread->context->rsp & 0xF))
        return false;
    VmRegion region{};
    uintptr_t stack_address = thread->context->rsp - 1;
    if (!process->vm_regions.find(stack_address, &region)) return false;
    bool stack_ok = region.type == VmRegionType::Stack &&
        (region.permissions & (VM_PERM_READ | VM_PERM_WRITE)) ==
            (VM_PERM_READ | VM_PERM_WRITE) &&
        !(region.permissions & VM_PERM_EXECUTE);
    VmRegionTable::release_snapshot(region);
    return stack_ok;
}
}

void user_process_test_fail_next_thread_allocation() {
    __atomic_store_n(&s_fail_next_thread_allocation, true, __ATOMIC_RELEASE);
}

UserProcessCreateResult user_process_create_from_elf(VFS::FileObject& file,
                                                      uint64_t file_offset,
                                                      uint64_t file_size,
                                                      UserProcessImage* output) {
    if (output) *output = {};
    if (!output) return create_result(UserProcessCreateError::InvalidArgument);

    Process* process = nullptr;
    ProcessResult process_result = Process::create_user(&process);
    if (process_result != ProcessResult::Success || !process) {
        UserProcessCreateResult result = create_result(UserProcessCreateError::ProcessCreationFailed);
        result.process_error = process_result;
        return result;
    }

    ElfImageInfo image{};
    ElfLoadResult loaded = elf_load_executable(file, file_offset, file_size,
                                                process->page_table_phys,
                                                process->vm_regions, &image);
    if (loaded.error != ElfLoadError::Success) {
        ProcessResult released = process->release();
        if (released != ProcessResult::Success)
            return create_result(UserProcessCreateError::CleanupFailed);
        UserProcessCreateResult result = create_result(UserProcessCreateError::ElfLoadFailed);
        result.elf_result = loaded;
        return result;
    }

    Thread* thread = nullptr;
    if (!__atomic_exchange_n(&s_fail_next_thread_allocation, false, __ATOMIC_ACQ_REL))
        thread = new Thread(process, reinterpret_cast<void(*)()>(image.entry_point),
                            true, "user-main");
    if (!thread) {
        cleanup_unpublished(process, nullptr);
        return create_result(UserProcessCreateError::ThreadAllocationFailed);
    }
    if (thread->initialization_result != ThreadInitResult::Success ||
        thread->state == THREAD_DEAD) {
        ThreadInitResult thread_error = thread->initialization_result;
        VmmMapResult mapping_error = thread->stack_mapping_error;
        VmRegionResult region_error = thread->stack_region_error;
        cleanup_unpublished(process, thread);
        UserProcessCreateResult result = create_result(
            UserProcessCreateError::ThreadInitializationFailed);
        result.thread_error = thread_error;
        result.thread_mapping_error = mapping_error;
        result.thread_region_error = region_error;
        return result;
    }
    if (!valid_initial_context(process, thread, image.entry_point)) {
        cleanup_unpublished(process, thread);
        return create_result(UserProcessCreateError::InvalidThreadContext);
    }

    uintptr_t entry_point = image.entry_point;
    uintptr_t initial_stack_pointer = thread->context->rsp;
    uint64_t mapped_memory_size = image.mapped_memory_size;
    uint32_t load_segment_count = image.load_segment_count;
    SchedulerPublishResult published = scheduler_add_thread(thread);
    if (published != SchedulerPublishResult::Success) {
        cleanup_unpublished(process, thread);
        UserProcessCreateResult result = create_result(
            UserProcessCreateError::SchedulerPublicationFailed);
        result.scheduler_error = published;
        return result;
    }

    output->process = process;
    output->entry_point = entry_point;
    output->initial_stack_pointer = initial_stack_pointer;
    output->mapped_memory_size = mapped_memory_size;
    output->load_segment_count = load_segment_count;
    return create_result(UserProcessCreateError::Success);
}
