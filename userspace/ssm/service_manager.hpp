#pragma once

#include <stddef.h>
#include <stdint.h>

namespace DrakOS::SSM {

constexpr uint32_t MaxServices = 32;
constexpr uint32_t MaxDependencies = 8;
constexpr uint32_t MaxArguments = 8;
constexpr uint32_t MaxEnvironment = 8;
constexpr size_t ServiceIdLength = 32;
constexpr size_t ServiceNameLength = 64;
constexpr size_t ExecutablePathLength = 128;
constexpr size_t ArgumentLength = 96;
constexpr size_t EnvironmentLength = 96;

enum class RestartPolicy : uint8_t { Never, OnFailure, Always };
enum class ServiceState : uint8_t {
    Registered,
    WaitingForDependencies,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

enum class ParseError : uint8_t {
    None,
    InvalidInput,
    LineTooLong,
    MissingVersion,
    UnsupportedVersion,
    InvalidSection,
    InvalidIdentifier,
    InvalidValue,
    UnknownKey,
    MissingRequiredField,
    MissingRequiredDependency,
    DuplicateService,
    CapacityExceeded,
};

enum class PlanError : uint8_t {
    None,
    InvalidInput,
    MissingRequiredDependency,
    DependencyCycle,
    CapacityExceeded,
};

enum class DependencyStatus : uint8_t {
    Ready,
    Waiting,
    RequiredDependencyFailed,
    InvalidInput,
};

struct Dependency {
    char service_id[ServiceIdLength];
    bool optional;
};

struct ServiceDefinition {
    char id[ServiceIdLength];
    char name[ServiceNameLength];
    char executable[ExecutablePathLength];
    char arguments[MaxArguments][ArgumentLength];
    char environment[MaxEnvironment][EnvironmentLength];
    Dependency dependencies[MaxDependencies];
    uint8_t argument_count;
    uint8_t environment_count;
    uint8_t dependency_count;
    bool optional_service;
    RestartPolicy restart_policy;
    uint32_t restart_limit;
    uint32_t restart_backoff_ms;
    uint32_t restart_backoff_max_ms;
    uint32_t startup_timeout_ms;
    uint32_t shutdown_timeout_ms;
};

struct ServiceRecord {
    ServiceDefinition definition;
    ServiceState state;
    uint64_t process_id;
    int32_t exit_status;
    uint32_t restart_count;
    bool intentional_stop;
};

struct ServiceRegistry {
    ServiceRecord services[MaxServices];
    uint32_t count;
};

struct ParseDiagnostic {
    ParseError error;
    uint32_t line;
};

struct StartupPlan {
    uint8_t service_indices[MaxServices];
    uint32_t count;
};

void registry_init(ServiceRegistry* registry);
bool parse_config(const char* text, size_t length, ServiceRegistry* registry,
                  ParseDiagnostic* diagnostic = nullptr);
PlanError build_startup_plan(const ServiceRegistry* registry, StartupPlan* plan);
DependencyStatus required_dependencies_status(const ServiceRegistry* registry,
                                             uint32_t service_index);
bool valid_transition(ServiceState from, ServiceState to);
bool transition(ServiceRecord* service, ServiceState next);
bool restart_allowed(const ServiceRecord* service, int32_t exit_status,
                     bool intentional_stop);
uint32_t restart_delay_ms(const ServiceDefinition* service, uint32_t restart_number);

} // namespace DrakOS::SSM
