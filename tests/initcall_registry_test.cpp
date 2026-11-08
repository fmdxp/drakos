#include "kernel/initcall_registry.hpp"
#include "module.hpp"

#include <stdio.h>

static int g_calls[8];
static int g_call_count;
static uint64_t g_clock_value;

static InitcallResult success_a() { g_calls[g_call_count++] = 1; return {true, nullptr}; }
static InitcallResult success_b() { g_calls[g_call_count++] = 2; return {true, nullptr}; }
static InitcallResult fail_optional() { g_calls[g_call_count++] = 3; return {false, "expected optional failure"}; }
static InitcallResult should_not_run() { g_calls[g_call_count++] = 4; return {true, nullptr}; }
static uint64_t fake_clock() { g_clock_value += 7; return g_clock_value; }

class RollbackModule : public KernelModule {
public:
    bool start() override { return false; }
    void stop() override { ++stops; }
    const char* get_name() const override { return "rollback-test"; }
    const char* failure_reason() const override { return "expected module failure"; }
    static int stops;
};

int RollbackModule::stops = 0;
REGISTER_MODULE_CONFIG(g_rollback_test, RollbackModule, 1_core,
                       InitcallRequirement::Optional, nullptr, 0)

static bool check(bool condition, const char* message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

static bool test_successful_topological_init() {
    static const char* const deps[] = {"prerequisite"};
    const InitcallDescriptor descriptors[] = {
        {"dependent", InitcallPhase::Core, InitcallRequirement::Mandatory, deps, 1, success_b},
        {"prerequisite", InitcallPhase::Core, InitcallRequirement::Mandatory, nullptr, 0, success_a},
    };
    InitcallRecord records[2];
    g_call_count = 0; g_clock_value = 0;
    InitcallSummary summary = initcall_run_all(descriptors, 2, records, 2, fake_clock);
    return check(summary.mandatory_failures == 0 && summary.succeeded == 2, "successful init summary") &&
           check(g_call_count == 2 && g_calls[0] == 1 && g_calls[1] == 2, "dependencies run before dependents") &&
           check(records[0].duration_ticks == 7 && records[1].duration_ticks == 7, "execution durations recorded");
}

static bool test_optional_failure() {
    const InitcallDescriptor descriptors[] = {
        {"optional", InitcallPhase::Core, InitcallRequirement::Optional, nullptr, 0, fail_optional},
    };
    InitcallRecord records[1];
    g_call_count = 0;
    InitcallSummary summary = initcall_run_all(descriptors, 1, records, 1);
    return check(summary.mandatory_failures == 0 && summary.optional_failures == 1, "optional failure does not fail boot") &&
           check(records[0].status == InitcallStatus::Failed && records[0].error, "optional failure is recorded");
}

static bool test_mandatory_failure() {
    const InitcallDescriptor descriptors[] = {
        {"mandatory", InitcallPhase::Core, InitcallRequirement::Mandatory, nullptr, 0, fail_optional},
    };
    InitcallRecord records[1];
    g_call_count = 0;
    InitcallSummary summary = initcall_run_all(descriptors, 1, records, 1);
    return check(summary.mandatory_failures == 1, "mandatory failure is fatal to boot") &&
           check(records[0].status == InitcallStatus::Failed, "mandatory failure status recorded");
}

static bool test_dependency_failure_propagation() {
    static const char* const deps[] = {"optional"};
    const InitcallDescriptor descriptors[] = {
        {"optional", InitcallPhase::Core, InitcallRequirement::Optional, nullptr, 0, fail_optional},
        {"consumer", InitcallPhase::Driver, InitcallRequirement::Mandatory, deps, 1, should_not_run},
    };
    InitcallRecord records[2];
    g_call_count = 0;
    InitcallSummary summary = initcall_run_all(descriptors, 2, records, 2);
    return check(summary.mandatory_failures == 1, "failed dependency propagates to mandatory consumer") &&
           check(records[1].status == InitcallStatus::SkippedDependency, "dependent is marked skipped") &&
           check(g_call_count == 1, "dependent callback is not executed");
}

static bool test_invalid_dependency() {
    static const char* const deps[] = {"missing"};
    const InitcallDescriptor descriptors[] = {
        {"consumer", InitcallPhase::Core, InitcallRequirement::Mandatory, deps, 1, should_not_run},
    };
    InitcallRecord records[1];
    g_call_count = 0;
    InitcallSummary summary = initcall_run_all(descriptors, 1, records, 1);
    return check(summary.mandatory_failures == 1, "missing dependency fails mandatory preflight") &&
           check(records[0].status == InitcallStatus::InvalidDependency, "missing dependency status recorded") &&
           check(g_call_count == 0, "invalid initcall callback is not executed");
}

static bool test_later_phase_dependency() {
    static const char* const deps[] = {"later"};
    const InitcallDescriptor descriptors[] = {
        {"early", InitcallPhase::Core, InitcallRequirement::Mandatory, deps, 1, should_not_run},
        {"later", InitcallPhase::Driver, InitcallRequirement::Optional, nullptr, 0, success_a},
    };
    InitcallRecord records[2];
    g_call_count = 0;
    InitcallSummary summary = initcall_run_all(descriptors, 2, records, 2);
    return check(summary.mandatory_failures == 1, "later-phase dependency is rejected") &&
           check(records[0].status == InitcallStatus::InvalidDependency, "later-phase dependency status recorded") &&
           check(g_call_count == 1 && g_calls[0] == 1, "only the independent later initcall executes");
}

static bool test_dependency_cycle() {
    static const char* const a_deps[] = {"b"};
    static const char* const b_deps[] = {"a"};
    const InitcallDescriptor descriptors[] = {
        {"a", InitcallPhase::Core, InitcallRequirement::Mandatory, a_deps, 1, should_not_run},
        {"b", InitcallPhase::Core, InitcallRequirement::Optional, b_deps, 1, success_a},
    };
    InitcallRecord records[2];
    g_call_count = 0;
    InitcallSummary summary = initcall_run_all(descriptors, 2, records, 2);
    return check(summary.mandatory_failures == 1 && summary.optional_failures == 1, "cycle is reported by requirement") &&
           check(records[0].status == InitcallStatus::DependencyCycle && records[1].status == InitcallStatus::DependencyCycle,
                 "all cycle members are marked") &&
           check(g_call_count == 0, "cycle members are not executed");
}

static bool test_module_failure_rolls_back() {
    RollbackModule::stops = 0;
    g_rollback_test = nullptr;
    InitcallResult result = __init_g_rollback_test();
    return check(!result.success && result.error, "failed module returns its reason") &&
           check(RollbackModule::stops == 1, "failed module invokes rollback") &&
           check(g_rollback_test == nullptr, "failed module is removed from its global slot");
}

int main() {
    bool ok = true;
    ok &= test_successful_topological_init();
    ok &= test_optional_failure();
    ok &= test_mandatory_failure();
    ok &= test_dependency_failure_propagation();
    ok &= test_invalid_dependency();
    ok &= test_later_phase_dependency();
    ok &= test_dependency_cycle();
    ok &= test_module_failure_rolls_back();
    if (!ok) return 1;
    puts("All initcall registry tests passed.");
    return 0;
}
