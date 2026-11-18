#pragma once

#include <stddef.h>
#include <stdint.h>

// Bounds are intentionally small while the initial stack is one page.
constexpr size_t USER_PROCESS_STACK_SIZE = 4096;
constexpr size_t USER_PROCESS_MAX_ARGUMENTS = 32;
constexpr size_t USER_PROCESS_MAX_ENVIRONMENT = 32;
constexpr size_t USER_PROCESS_MAX_STRING_LENGTH = 256; // Including NUL.
constexpr size_t USER_PROCESS_MAX_STRINGS_SIZE = 2048; // Including NUL bytes.

struct UserProcessArguments {
    const char* const* argv;
    size_t argc;
    const char* const* envp;
    size_t envc;
};

enum class UserStackBuildResult : uint8_t {
    Success,
    InvalidArguments,
    CountLimitExceeded,
    StringTooLong,
    StringStorageLimitExceeded,
    StackTooSmall,
};

// Builds the initial System V AMD64 process-entry stack in an already mapped
// stack page. Input strings and pointer arrays are kernel-owned inputs; their
// contents are copied and only resulting user virtual addresses enter the
// user's stack. A null page_bytes performs validation only.
inline UserStackBuildResult user_stack_build(uint8_t* page_bytes, size_t page_size,
                                             uintptr_t user_page_address,
                                             const UserProcessArguments& args,
                                             uintptr_t* initial_stack_pointer) {
    if (initial_stack_pointer) *initial_stack_pointer = 0;
    if (!initial_stack_pointer || (args.argc && !args.argv) ||
        (args.envc && !args.envp))
        return UserStackBuildResult::InvalidArguments;
    if (args.argc > USER_PROCESS_MAX_ARGUMENTS ||
        args.envc > USER_PROCESS_MAX_ENVIRONMENT)
        return UserStackBuildResult::CountLimitExceeded;

    size_t strings_size = 0;
    for (size_t group = 0; group < 2; ++group) {
        const char* const* strings = group ? args.envp : args.argv;
        size_t count = group ? args.envc : args.argc;
        for (size_t i = 0; i < count; ++i) {
            const char* string = strings[i];
            if (!string) return UserStackBuildResult::InvalidArguments;
            size_t length = 0;
            while (length < USER_PROCESS_MAX_STRING_LENGTH && string[length]) ++length;
            if (length == USER_PROCESS_MAX_STRING_LENGTH)
                return UserStackBuildResult::StringTooLong;
            ++length; // Include NUL.
            if (length > USER_PROCESS_MAX_STRINGS_SIZE - strings_size)
                return UserStackBuildResult::StringStorageLimitExceeded;
            strings_size += length;
        }
    }

    constexpr size_t word = sizeof(uint64_t);
    size_t frame_words = 5 + args.argc + args.envc;
    constexpr uintptr_t USER_LIMIT = 1ULL << 47;
    if ((user_page_address & (USER_PROCESS_STACK_SIZE - 1)) ||
        user_page_address >= USER_LIMIT ||
        page_size > USER_LIMIT - user_page_address ||
        frame_words > SIZE_MAX / word || strings_size > page_size ||
        frame_words * word > page_size - strings_size ||
        user_page_address > UINTPTR_MAX - page_size)
        return UserStackBuildResult::StackTooSmall;

    if (!page_bytes) return UserStackBuildResult::Success;

    size_t string_cursor = page_size;
    size_t frame_bytes = frame_words * word;
    size_t expected_string_cursor = page_size - strings_size;
    size_t frame_start = (expected_string_cursor - frame_bytes) &
                         ~static_cast<size_t>(0xF);
    if (frame_start > expected_string_cursor ||
        frame_bytes > expected_string_cursor - frame_start)
        return UserStackBuildResult::StackTooSmall;
    auto* frame = reinterpret_cast<uint64_t*>(page_bytes + frame_start);

    // Store the strings from the top down. The initial page is zeroed by the
    // caller, so alignment padding and unused bytes remain deterministic.
    auto copy_group = [&](const char* const* strings, size_t count,
                          size_t pointer_word) -> bool {
        for (size_t i = 0; i < count; ++i) {
            size_t length = 1;
            while (strings[i][length - 1]) ++length;
            if (length > string_cursor) return false;
            string_cursor -= length;
            uintptr_t address = user_page_address + string_cursor;
            frame[pointer_word + i] = address;
            for (size_t j = 0; j < length; ++j)
                page_bytes[string_cursor + j] = static_cast<uint8_t>(strings[i][j]);
        }
        return true;
    };

    const size_t argv_word = 1;
    const size_t argv_null_word = argv_word + args.argc;
    const size_t envp_word = argv_null_word + 1;
    const size_t envp_null_word = envp_word + args.envc;
    const size_t auxv_word = envp_null_word + 1;
    if (!copy_group(args.argv, args.argc, argv_word) ||
        !copy_group(args.envp, args.envc, envp_word))
        return UserStackBuildResult::StackTooSmall;

    if (string_cursor != expected_string_cursor)
        return UserStackBuildResult::StackTooSmall;
    frame[0] = args.argc;
    frame[argv_null_word] = 0;
    frame[envp_null_word] = 0;
    frame[auxv_word] = 0;     // AT_NULL
    frame[auxv_word + 1] = 0; // Auxiliary-vector value
    *initial_stack_pointer = user_page_address + frame_start;
    return UserStackBuildResult::Success;
}
