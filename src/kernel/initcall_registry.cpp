#include "kernel/initcall_registry.hpp"

static int find_record(const InitcallRecord* records, size_t count, const char* name) {
    if (!name) return -1;
    for (size_t i = 0; i < count; ++i) {
        const char* candidate = records[i].descriptor ? records[i].descriptor->name : nullptr;
        if (!candidate) continue;
        size_t n = 0;
        while (candidate[n] && name[n] && candidate[n] == name[n]) ++n;
        if (candidate[n] == '\0' && name[n] == '\0') return static_cast<int>(i);
    }
    return -1;
}

static void set_error(InitcallRecord& record, InitcallStatus status, const char* error) {
    record.status = status;
    record.error = error;
}

static void mark_cycles(InitcallRecord* records, size_t count) {
    uint8_t color[INITCALL_MAX_COUNT] = {};
    size_t stack[INITCALL_MAX_COUNT] = {};

    auto visit = [&](auto&& self, size_t index, size_t& stack_size) -> void {
        if (records[index].status != InitcallStatus::Pending) return;
        color[index] = 1;
        stack[stack_size++] = index;

        const InitcallDescriptor& descriptor = *records[index].descriptor;
        for (size_t d = 0; d < descriptor.dependency_count; ++d) {
            int dep = find_record(records, count, descriptor.dependencies[d]);
            if (dep < 0 || records[dep].status != InitcallStatus::Pending) continue;
            if (color[dep] == 0) {
                self(self, static_cast<size_t>(dep), stack_size);
            } else if (color[dep] == 1) {
                size_t first = 0;
                while (first < stack_size && stack[first] != static_cast<size_t>(dep)) ++first;
                for (size_t j = first; j < stack_size; ++j) {
                    set_error(records[stack[j]], InitcallStatus::DependencyCycle,
                              "dependency cycle detected");
                }
            }
        }

        --stack_size;
        color[index] = 2;
    };

    for (size_t i = 0; i < count; ++i) {
        if (records[i].status == InitcallStatus::Pending && color[i] == 0) {
            size_t stack_size = 0;
            visit(visit, i, stack_size);
        }
    }
}

InitcallSummary initcall_run_all(const InitcallDescriptor* descriptors,
                                 size_t count,
                                 InitcallRecord* records,
                                 size_t record_capacity,
                                 InitcallClock clock) {
    InitcallSummary summary = {0, 0, 0, 0};
    if (!descriptors || !records || count == 0 || count > INITCALL_MAX_COUNT || count > record_capacity) {
        summary.mandatory_failures = 1;
        return summary;
    }

    summary.count = count;
    for (size_t i = 0; i < count; ++i) {
        records[i] = {&descriptors[i], InitcallStatus::Pending, 0, nullptr};
    }

    // Validate entries and dependency names before running any component.
    for (size_t i = 0; i < count; ++i) {
        const InitcallDescriptor& descriptor = descriptors[i];
        if (!descriptor.name || !descriptor.name[0] || !descriptor.run ||
            (descriptor.dependency_count != 0 && !descriptor.dependencies)) {
            set_error(records[i], InitcallStatus::InvalidDependency, "invalid initcall descriptor");
            continue;
        }

        for (size_t j = 0; j < i; ++j) {
            if (descriptors[j].name && find_record(records, i, descriptor.name) >= 0) {
                set_error(records[i], InitcallStatus::InvalidDependency, "duplicate initcall name");
                set_error(records[j], InitcallStatus::InvalidDependency, "duplicate initcall name");
            }
        }

        for (size_t d = 0; d < descriptor.dependency_count; ++d) {
            int dep = find_record(records, count, descriptor.dependencies[d]);
            if (dep < 0) {
                set_error(records[i], InitcallStatus::InvalidDependency, "required dependency is not registered");
                break;
            }
            if (static_cast<uint8_t>(records[dep].descriptor->phase) > static_cast<uint8_t>(descriptor.phase)) {
                set_error(records[i], InitcallStatus::InvalidDependency, "dependency is in a later init phase");
                break;
            }
        }
    }

    mark_cycles(records, count);

    // Preserve phase ordering while topologically resolving dependencies within each phase.
    for (uint8_t phase = static_cast<uint8_t>(InitcallPhase::Core);
         phase <= static_cast<uint8_t>(InitcallPhase::Bus); ++phase) {
        bool made_progress;
        do {
            made_progress = false;
            for (size_t i = 0; i < count; ++i) {
                InitcallRecord& record = records[i];
                if (record.status != InitcallStatus::Pending ||
                    static_cast<uint8_t>(record.descriptor->phase) != phase) continue;

                bool waiting = false;
                bool failed_dependency = false;
                for (size_t d = 0; d < record.descriptor->dependency_count; ++d) {
                    int dep = find_record(records, count, record.descriptor->dependencies[d]);
                    if (dep < 0) {
                        failed_dependency = true;
                        record.error = "required dependency is not registered";
                        break;
                    }
                    InitcallStatus dep_status = records[dep].status;
                    if (dep_status == InitcallStatus::Pending) waiting = true;
                    else if (dep_status != InitcallStatus::Succeeded) {
                        failed_dependency = true;
                        record.error = "required dependency did not initialize";
                        break;
                    }
                }

                if (failed_dependency) {
                    record.status = InitcallStatus::SkippedDependency;
                    made_progress = true;
                    continue;
                }
                if (waiting) continue;

                uint64_t started = clock ? clock() : 0;
                InitcallResult result = record.descriptor->run();
                uint64_t finished = clock ? clock() : started;
                record.duration_ticks = finished >= started ? finished - started : 0;
                record.status = result.success ? InitcallStatus::Succeeded : InitcallStatus::Failed;
                record.error = result.success ? nullptr : (result.error ? result.error : "initialization failed");
                made_progress = true;
            }
        } while (made_progress);
    }

    for (size_t i = 0; i < count; ++i) {
        InitcallRecord& record = records[i];
        if (record.status == InitcallStatus::Pending) {
            set_error(record, InitcallStatus::DependencyCycle, "unresolved dependency cycle");
        }
        if (record.status == InitcallStatus::Succeeded) {
            ++summary.succeeded;
        } else if (record.descriptor->requirement == InitcallRequirement::Mandatory) {
            ++summary.mandatory_failures;
        } else {
            ++summary.optional_failures;
        }
    }
    return summary;
}

const char* initcall_status_name(InitcallStatus status) {
    switch (status) {
        case InitcallStatus::Pending: return "pending";
        case InitcallStatus::Succeeded: return "ok";
        case InitcallStatus::Failed: return "failed";
        case InitcallStatus::SkippedDependency: return "skipped (dependency)";
        case InitcallStatus::InvalidDependency: return "invalid dependency";
        case InitcallStatus::DependencyCycle: return "dependency cycle";
    }
    return "unknown";
}
