#include "../userspace/ssm/service_manager.hpp"

#include <cstdio>
#include <cstring>

using namespace DrakOS::SSM;

static bool check(bool condition, const char* name) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

static bool parse(const char* text, ServiceRegistry& registry, ParseDiagnostic& diagnostic) {
    return parse_config(text, std::strlen(text), &registry, &diagnostic);
}

int main() {
    bool ok = true;
    const char valid_config[] =
        "# drakos service configuration\n"
        "version = 1\n"
        "[service app]\n"
        "name = \"Example App\"\n"
        "exec = /bin/app.drk\n"
        "arg = --safe-mode\n"
        "arg = \"hello world\"\n"
        "env = MODE=production\n"
        "depends = !storage\n"
        "depends = ?metrics\n"
        "restart = on-failure\n"
        "restart-limit = 4\n"
        "restart-backoff-ms = 100\n"
        "restart-backoff-max-ms = 500\n"
        "startup-timeout-ms = 2000\n"
        "shutdown-timeout-ms = 1000\n"
        "\n"
        "[service storage]\n"
        "name = Storage\n"
        "exec = /bin/storage.drk\n"
        "restart = always\n"
        "restart-limit = 3\n";
    ServiceRegistry registry;
    registry_init(&registry);
    ParseDiagnostic diagnostic{};
    ok &= check(parse(valid_config, registry, diagnostic), "versioned service config parses");
    ok &= check(registry.count == 2 && registry.services[0].definition.argument_count == 2 &&
        std::strcmp(registry.services[0].definition.arguments[1], "hello world") == 0 &&
        registry.services[0].definition.environment_count == 1,
        "arguments, environment, and readable names are preserved");
    ok &= check(registry.services[0].definition.restart_policy == RestartPolicy::OnFailure &&
        registry.services[0].definition.restart_limit == 4 &&
        registry.services[0].definition.startup_timeout_ms == 2000,
        "restart and timeout settings are loaded");

    StartupPlan plan{};
    ok &= check(build_startup_plan(&registry, &plan) == PlanError::None && plan.count == 2 &&
        std::strcmp(registry.services[plan.service_indices[0]].definition.id, "storage") == 0 &&
        std::strcmp(registry.services[plan.service_indices[1]].definition.id, "app") == 0,
        "dependency plan starts required dependencies first and skips absent optional ones");
    ok &= check(required_dependencies_status(&registry, 0) == DependencyStatus::Waiting,
        "required dependency gates startup until running");
    registry.services[1].state = ServiceState::Running;
    ok &= check(required_dependencies_status(&registry, 0) == DependencyStatus::Ready,
        "running required dependency permits startup");
    registry.services[1].state = ServiceState::Failed;
    ok &= check(required_dependencies_status(&registry, 0) == DependencyStatus::RequiredDependencyFailed,
        "failed required dependency blocks startup");
    registry.services[1].state = ServiceState::Registered;
    ok &= check(required_dependencies_status(&registry, 1) == DependencyStatus::Ready,
        "missing optional dependency does not block startup");

    const char missing_required[] =
        "version=1\n[service app]\nname=App\nexec=/bin/app\ndepends=!missing\n";
    ok &= check(!parse(missing_required, registry, diagnostic) &&
        diagnostic.error == ParseError::MissingRequiredDependency,
        "missing required dependency is reported explicitly");

    const char cycle[] =
        "version=1\n"
        "[service alpha]\nname=Alpha\nexec=/bin/a\ndepends=!beta\n"
        "[service beta]\nname=Beta\nexec=/bin/b\ndepends=!alpha\n";
    ok &= check(parse(cycle, registry, diagnostic) &&
        build_startup_plan(&registry, &plan) == PlanError::DependencyCycle,
        "dependency cycles are detected");

    const char duplicate[] =
        "version=1\n"
        "[service same]\nname=One\nexec=/bin/one\n"
        "[service same]\nname=Two\nexec=/bin/two\n";
    ok &= check(!parse(duplicate, registry, diagnostic) &&
        diagnostic.error == ParseError::DuplicateService,
        "duplicate service identifiers are rejected");

    const char unsupported[] = "version=2\n[service app]\nname=App\nexec=/bin/app\n";
    ok &= check(!parse(unsupported, registry, diagnostic) &&
        diagnostic.error == ParseError::UnsupportedVersion,
        "unsupported configuration versions are rejected");
    const char unknown_key[] =
        "version=1\n[service app]\nname=App\nexec=/bin/app\nwat=1\n";
    ok &= check(!parse(unknown_key, registry, diagnostic) &&
        diagnostic.error == ParseError::UnknownKey,
        "unknown service fields are not silently ignored");

    ServiceRecord service{};
    service.state = ServiceState::Registered;
    service.definition.restart_policy = RestartPolicy::OnFailure;
    service.definition.restart_limit = 2;
    service.definition.restart_backoff_ms = 100;
    service.definition.restart_backoff_max_ms = 350;
    ok &= check(transition(&service, ServiceState::Starting) &&
        transition(&service, ServiceState::Running) &&
        !transition(&service, ServiceState::Registered),
        "service state transitions accept valid changes and reject invalid changes");
    ok &= check(restart_allowed(&service, 1, false) && !restart_allowed(&service, 0, false) &&
        !restart_allowed(&service, 1, true),
        "on-failure restart policy distinguishes success and intentional stop");
    service.restart_count = 2;
    ok &= check(!restart_allowed(&service, 1, false), "restart limit prevents unbounded retries");
    ok &= check(restart_delay_ms(&service.definition, 1) == 100 &&
        restart_delay_ms(&service.definition, 2) == 200 &&
        restart_delay_ms(&service.definition, 3) == 350,
        "restart backoff doubles and saturates at its configured maximum");

    if (!ok) return 1;
    std::puts("SSM configuration, dependency, lifecycle, and restart-policy tests passed.");
    return 0;
}
