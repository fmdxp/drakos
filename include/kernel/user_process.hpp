#pragma once

#include <stdint.h>

#include "kernel/elf_loader.hpp"
#include "kernel/thread.hpp"

namespace VFS { class FileObject; }

enum class UserProcessCreateError : uint8_t {
    Success,
    InvalidArgument,
    ProcessCreationFailed,
    ElfLoadFailed,
    ThreadAllocationFailed,
    ThreadInitializationFailed,
    InvalidThreadContext,
    SchedulerPublicationFailed,
    InvalidArguments,
    ArgumentLimitExceeded,
    ArgumentStorageExceeded,
    CleanupFailed,
    InvalidParent,
};

struct UserProcessImage {
    Process* process;
    uintptr_t entry_point;
    uintptr_t initial_stack_pointer;
    uint64_t mapped_memory_size;
    uint32_t load_segment_count;
};

struct UserProcessCreateResult {
    UserProcessCreateError error;
    ProcessResult process_error;
    ElfLoadResult elf_result;
    ThreadInitResult thread_error;
    VmmMapResult thread_mapping_error;
    VmRegionResult thread_region_error;
    SchedulerPublishResult scheduler_error;
};

// Creates a user process, loads an ELF image into it, builds the initial user
// stack and thread context, then publishes the thread. The caller retains the
// FileObject and receives the process's creator reference on success.
UserProcessCreateResult user_process_create_from_elf(VFS::FileObject& file,
                                                      uint64_t file_offset,
                                                      uint64_t file_size,
                                                      UserProcessImage* output,
                                                      const UserProcessArguments* arguments = nullptr,
                                                      Process* parent = nullptr);

// Test seam used by QEMU runtime tests to exercise thread-allocation rollback.
void user_process_test_fail_next_thread_allocation();
