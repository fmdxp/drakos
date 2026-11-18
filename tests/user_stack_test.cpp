#include "kernel/user_stack.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace {
constexpr uintptr_t kUserPage = 0x700000;
constexpr size_t kPageSize = 4096;

bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: user stack: %s\n", message);
    return condition;
}

bool has_string(const uint8_t* page, uintptr_t address, const char* expected) {
    if (address < kUserPage || address >= kUserPage + kPageSize) return false;
    const char* actual = reinterpret_cast<const char*>(page + (address - kUserPage));
    return std::strcmp(actual, expected) == 0;
}
}

int main() {
    bool ok = true;
    std::array<uint8_t, kPageSize> page{};
    uintptr_t rsp = 0;

    UserProcessArguments empty{};
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, empty, &rsp) ==
                    UserStackBuildResult::Success,
                "empty argument/environment stack builds");
    auto* words = reinterpret_cast<const uint64_t*>(page.data() + (rsp - kUserPage));
    ok &= check(!(rsp & 15) && words[0] == 0 && words[1] == 0 && words[2] == 0 &&
                    words[3] == 0 && words[4] == 0,
                "empty frame is aligned and terminated");

    const char* one_argv[] = {"tool"};
    const char* one_env[] = {"A=1"};
    UserProcessArguments one{one_argv, 1, one_env, 1};
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, one, &rsp) ==
                    UserStackBuildResult::Success,
                "one argument and environment entry build");
    words = reinterpret_cast<const uint64_t*>(page.data() + (rsp - kUserPage));
    ok &= check(!(rsp & 15) && words[0] == 1 && words[1] && words[2] == 0 &&
                    words[3] && words[4] == 0 && words[5] == 0 && words[6] == 0,
                "one-entry argv/envp and auxv terminators are positioned correctly");
    ok &= check(has_string(page.data(), words[1], "tool") &&
                    has_string(page.data(), words[3], "A=1"),
                "one-entry pointers refer to copied strings");

    const char* many_argv[] = {"first", "second", "third"};
    const char* many_env[] = {"MODE=debug", "PATH=/bin"};
    UserProcessArguments many{many_argv, 3, many_env, 2};
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, many, &rsp) ==
                    UserStackBuildResult::Success,
                "multiple argument and environment entries build");
    words = reinterpret_cast<const uint64_t*>(page.data() + (rsp - kUserPage));
    ok &= check(!(rsp & 15) && words[0] == 3 && words[4] == 0 &&
                    words[5] && words[6] && words[7] == 0 &&
                    words[8] == 0 && words[9] == 0,
                "multiple pointer arrays and auxv terminator are correct");
    ok &= check(has_string(page.data(), words[1], "first") &&
                    has_string(page.data(), words[2], "second") &&
                    has_string(page.data(), words[3], "third") &&
                    has_string(page.data(), words[5], "MODE=debug") &&
                    has_string(page.data(), words[6], "PATH=/bin"),
                "multiple pointers refer to the expected copied strings");

    uintptr_t unchanged_rsp = 0x1234;
    UserProcessArguments missing_array{nullptr, 1, nullptr, 0};
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, missing_array,
                                 &unchanged_rsp) == UserStackBuildResult::InvalidArguments &&
                    unchanged_rsp == 0,
                "null array with nonzero count is rejected");
    const char* null_string[] = {nullptr};
    UserProcessArguments missing_string{null_string, 1, nullptr, 0};
    page.fill(0xA5);
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, missing_string,
                                 &rsp) == UserStackBuildResult::InvalidArguments &&
                    page[0] == 0xA5 && page[kPageSize - 1] == 0xA5,
                "null string is rejected before modifying stack storage");

    const char* too_many[USER_PROCESS_MAX_ARGUMENTS + 1]{};
    for (auto& item : too_many) item = "x";
    UserProcessArguments over_count{too_many, USER_PROCESS_MAX_ARGUMENTS + 1, nullptr, 0};
    page.fill(0x5A);
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, over_count, &rsp) ==
                    UserStackBuildResult::CountLimitExceeded && page[0] == 0x5A,
                "argument count limit rejects without mutation");
    const char* max_argv_count[USER_PROCESS_MAX_ARGUMENTS];
    const char* max_env_count[USER_PROCESS_MAX_ENVIRONMENT];
    for (auto& item : max_argv_count) item = "a";
    for (auto& item : max_env_count) item = "E=1";
    UserProcessArguments boundary_counts{max_argv_count, USER_PROCESS_MAX_ARGUMENTS,
                                         max_env_count, USER_PROCESS_MAX_ENVIRONMENT};
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, boundary_counts, &rsp) ==
                    UserStackBuildResult::Success,
                "maximum argument and environment counts are accepted");
    const char* too_many_env[USER_PROCESS_MAX_ENVIRONMENT + 1]{};
    for (auto& item : too_many_env) item = "E=1";
    UserProcessArguments over_env_count{nullptr, 0, too_many_env,
                                        USER_PROCESS_MAX_ENVIRONMENT + 1};
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, over_env_count, &rsp) ==
                    UserStackBuildResult::CountLimitExceeded,
                "environment count limit is enforced");

    char long_string[USER_PROCESS_MAX_STRING_LENGTH + 1];
    std::memset(long_string, 'x', sizeof(long_string));
    long_string[sizeof(long_string) - 1] = 0;
    const char* long_argv[] = {long_string};
    UserProcessArguments over_length{long_argv, 1, nullptr, 0};
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, over_length, &rsp) ==
                    UserStackBuildResult::StringTooLong,
                "overlong string is rejected");

    char maximum_string[USER_PROCESS_MAX_STRING_LENGTH];
    std::memset(maximum_string, 'm', sizeof(maximum_string) - 1);
    maximum_string[sizeof(maximum_string) - 1] = 0;
    const char* max_argv[] = {maximum_string};
    UserProcessArguments max_length{max_argv, 1, nullptr, 0};
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, max_length, &rsp) ==
                    UserStackBuildResult::Success,
                "maximum permitted string length is accepted");

    char over_storage[250];
    std::memset(over_storage, 's', sizeof(over_storage) - 1);
    over_storage[sizeof(over_storage) - 1] = 0;
    const char* storage_argv[9];
    for (auto& item : storage_argv) item = over_storage;
    UserProcessArguments over_storage_args{storage_argv, 9, nullptr, 0};
    page.fill(0xC3);
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage, over_storage_args,
                                 &rsp) == UserStackBuildResult::StringStorageLimitExceeded &&
                    page[0] == 0xC3,
                "aggregate string-storage limit rejects before mutation");

    std::array<uint8_t, 64> tiny_page{};
    ok &= check(user_stack_build(tiny_page.data(), tiny_page.size(), kUserPage, many, &rsp) ==
                    UserStackBuildResult::StackTooSmall,
                "insufficient stack capacity is rejected");
    ok &= check(user_stack_build(page.data(), page.size(), kUserPage + 1, empty, &rsp) ==
                    UserStackBuildResult::StackTooSmall &&
                    user_stack_build(page.data(), page.size(), 1ULL << 47, empty, &rsp) ==
                    UserStackBuildResult::StackTooSmall,
                "unaligned and non-user stack address ranges are rejected");
    std::puts(ok ? "user stack tests: PASS" : "user stack tests: FAIL");
    return ok ? 0 : 1;
}
