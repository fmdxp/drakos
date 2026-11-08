#pragma once

#include <stddef.h>
#include <stdint.h>

enum class InitcallPhase : uint8_t {
    Core = 0,
    MemoryA = 1,
    MemoryB = 2,
    MemoryC = 3,
    Firmware = 4,
    Driver = 5,
    Device = 6,
    Bus = 7,
};

enum class InitcallRequirement : uint8_t {
    Optional,
    Mandatory,
};

enum class InitcallStatus : uint8_t {
    Pending,
    Succeeded,
    Failed,
    SkippedDependency,
    InvalidDependency,
    DependencyCycle,
};

struct InitcallResult {
    bool success;
    const char* error;
};

using InitcallRunner = InitcallResult (*)();
using InitcallClock = uint64_t (*)();

struct InitcallDescriptor {
    const char* name;
    InitcallPhase phase;
    InitcallRequirement requirement;
    const char* const* dependencies;
    size_t dependency_count;
    InitcallRunner run;
};

struct InitcallRecord {
    const InitcallDescriptor* descriptor;
    InitcallStatus status;
    uint64_t duration_ticks;
    const char* error;
};

struct InitcallSummary {
    size_t count;
    size_t succeeded;
    size_t optional_failures;
    size_t mandatory_failures;
};

constexpr size_t INITCALL_MAX_COUNT = 64;

InitcallSummary initcall_run_all(const InitcallDescriptor* descriptors,
                                 size_t count,
                                 InitcallRecord* records,
                                 size_t record_capacity,
                                 InitcallClock clock = nullptr);

const char* initcall_status_name(InitcallStatus status);
