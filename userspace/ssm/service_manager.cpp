#include "service_manager.hpp"

namespace DrakOS::SSM {

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static size_t text_length(const char* value) {
    size_t length = 0;
    if (value) while (value[length]) ++length;
    return length;
}

static bool equal(const char* left, const char* right) {
    size_t i = 0;
    while (left[i] && right[i] && left[i] == right[i]) ++i;
    return left[i] == right[i];
}

static char* trim(char* value) {
    while (*value && is_space(*value)) ++value;
    size_t length = text_length(value);
    while (length && is_space(value[length - 1])) value[--length] = 0;
    return value;
}

static bool copy_string(char* output, size_t capacity, const char* value) {
    if (!output || !capacity || !value) return false;
    size_t length = text_length(value);
    if (!length || length >= capacity) return false;
    for (size_t i = 0; i <= length; ++i) output[i] = value[i];
    return true;
}

static bool valid_id(const char* id) {
    size_t length = text_length(id);
    if (!length || length >= ServiceIdLength) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              is_digit(c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

static void initialize_definition(ServiceDefinition& definition) {
    auto* bytes = reinterpret_cast<uint8_t*>(&definition);
    for (size_t i = 0; i < sizeof(definition); ++i) bytes[i] = 0;
    definition.restart_policy = RestartPolicy::Never;
    definition.restart_limit = 5;
    definition.restart_backoff_ms = 250;
    definition.restart_backoff_max_ms = 30000;
    definition.startup_timeout_ms = 5000;
    definition.shutdown_timeout_ms = 3000;
}

void registry_init(ServiceRegistry* registry) {
    if (!registry) return;
    auto* bytes = reinterpret_cast<uint8_t*>(registry);
    for (size_t i = 0; i < sizeof(*registry); ++i) bytes[i] = 0;
}

static int32_t find_service(const ServiceRegistry& registry, const char* id) {
    for (uint32_t i = 0; i < registry.count; ++i)
        if (equal(registry.services[i].definition.id, id)) return static_cast<int32_t>(i);
    return -1;
}

static bool parse_u32(const char* text, uint32_t& output) {
    if (!text || !*text) return false;
    uint32_t value = 0;
    for (size_t i = 0; text[i]; ++i) {
        if (!is_digit(text[i])) return false;
        uint32_t digit = static_cast<uint32_t>(text[i] - '0');
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    output = value;
    return true;
}

static bool decode_value(char* value) {
    value = trim(value);
    size_t length = text_length(value);
    if (!length) return false;
    if (value[0] != '"') return true;
    if (length < 2 || value[length - 1] != '"') return false;
    size_t output = 0;
    for (size_t i = 1; i + 1 < length; ++i) {
        char c = value[i];
        if (c == '\\') {
            if (++i + 1 >= length || (value[i] != '\\' && value[i] != '"')) return false;
            c = value[i];
        } else if (c == '"') {
            return false;
        }
        value[output++] = c;
    }
    value[output] = 0;
    return true;
}

static ParseError append_dependency(ServiceDefinition& definition, char* value) {
    if (definition.dependency_count == MaxDependencies) return ParseError::CapacityExceeded;
    bool optional = false;
    if (value[0] == '?') { optional = true; ++value; }
    else if (value[0] == '!') ++value;
    if (!valid_id(value)) return ParseError::InvalidIdentifier;
    Dependency& dependency = definition.dependencies[definition.dependency_count];
    if (!copy_string(dependency.service_id, sizeof(dependency.service_id), value))
        return ParseError::InvalidValue;
    dependency.optional = optional;
    ++definition.dependency_count;
    return ParseError::None;
}

static ParseError parse_field(ServiceDefinition& definition, char* key, char* value) {
    if (!decode_value(value)) return ParseError::InvalidValue;
    if (equal(key, "name"))
        return copy_string(definition.name, sizeof(definition.name), value)
            ? ParseError::None : ParseError::InvalidValue;
    if (equal(key, "exec")) {
        if (value[0] != '/') return ParseError::InvalidValue;
        return copy_string(definition.executable, sizeof(definition.executable), value)
            ? ParseError::None : ParseError::InvalidValue;
    }
    if (equal(key, "arg")) {
        if (definition.argument_count == MaxArguments) return ParseError::CapacityExceeded;
        if (!copy_string(definition.arguments[definition.argument_count], ArgumentLength, value))
            return ParseError::InvalidValue;
        ++definition.argument_count;
        return ParseError::None;
    }
    if (equal(key, "env")) {
        if (definition.environment_count == MaxEnvironment) return ParseError::CapacityExceeded;
        size_t length = text_length(value);
        size_t separator = 0;
        while (separator < length && value[separator] != '=') ++separator;
        if (!separator || separator + 1 >= length) return ParseError::InvalidValue;
        if (!copy_string(definition.environment[definition.environment_count], EnvironmentLength, value))
            return ParseError::InvalidValue;
        ++definition.environment_count;
        return ParseError::None;
    }
    if (equal(key, "depends")) return append_dependency(definition, value);
    if (equal(key, "optional")) {
        if (equal(value, "true")) definition.optional_service = true;
        else if (equal(value, "false")) definition.optional_service = false;
        else return ParseError::InvalidValue;
        return ParseError::None;
    }
    if (equal(key, "restart")) {
        if (equal(value, "never")) definition.restart_policy = RestartPolicy::Never;
        else if (equal(value, "on-failure")) definition.restart_policy = RestartPolicy::OnFailure;
        else if (equal(value, "always")) definition.restart_policy = RestartPolicy::Always;
        else return ParseError::InvalidValue;
        return ParseError::None;
    }
    uint32_t parsed = 0;
    if (equal(key, "restart-limit")) {
        if (!parse_u32(value, parsed)) return ParseError::InvalidValue;
        definition.restart_limit = parsed;
        return ParseError::None;
    }
    if (equal(key, "restart-backoff-ms")) {
        if (!parse_u32(value, parsed)) return ParseError::InvalidValue;
        definition.restart_backoff_ms = parsed;
        return ParseError::None;
    }
    if (equal(key, "restart-backoff-max-ms")) {
        if (!parse_u32(value, parsed)) return ParseError::InvalidValue;
        definition.restart_backoff_max_ms = parsed;
        return ParseError::None;
    }
    if (equal(key, "startup-timeout-ms")) {
        if (!parse_u32(value, parsed) || !parsed) return ParseError::InvalidValue;
        definition.startup_timeout_ms = parsed;
        return ParseError::None;
    }
    if (equal(key, "shutdown-timeout-ms")) {
        if (!parse_u32(value, parsed) || !parsed) return ParseError::InvalidValue;
        definition.shutdown_timeout_ms = parsed;
        return ParseError::None;
    }
    return ParseError::UnknownKey;
}

static bool complete_definition(const ServiceDefinition& definition) {
    return definition.id[0] && definition.name[0] && definition.executable[0] &&
        definition.restart_backoff_ms <= definition.restart_backoff_max_ms;
}

bool parse_config(const char* text, size_t length, ServiceRegistry* output,
                  ParseDiagnostic* diagnostic) {
    if (diagnostic) *diagnostic = {ParseError::None, 0};
    if (!text || !output || !length) {
        if (diagnostic) diagnostic->error = ParseError::InvalidInput;
        return false;
    }
    ServiceRegistry parsed;
    registry_init(&parsed);
    ServiceDefinition current;
    initialize_definition(current);
    bool has_section = false;
    bool has_version = false;
    uint32_t line_number = 0;
    size_t offset = 0;
    char line[256];

    auto fail = [&](ParseError error) {
        if (diagnostic) *diagnostic = {error, line_number};
        return false;
    };
    auto finish_section = [&]() -> ParseError {
        if (!has_section) return ParseError::None;
        if (!complete_definition(current)) return ParseError::MissingRequiredField;
        for (uint32_t i = 0; i < current.dependency_count; ++i) {
            for (uint32_t j = 0; j < i; ++j)
                if (equal(current.dependencies[i].service_id, current.dependencies[j].service_id))
                    return ParseError::InvalidValue;
        }
        if (parsed.count == MaxServices) return ParseError::CapacityExceeded;
        parsed.services[parsed.count++] = {current, ServiceState::Registered, 0, 0, 0, false};
        return ParseError::None;
    };

    while (offset < length) {
        ++line_number;
        size_t line_length = 0;
        while (offset < length && text[offset] != '\n') {
            if (line_length + 1 >= sizeof(line)) return fail(ParseError::LineTooLong);
            line[line_length++] = text[offset++];
        }
        if (offset < length && text[offset] == '\n') ++offset;
        line[line_length] = 0;
        char* content = trim(line);
        if (!*content || *content == '#') continue;

        if (*content == '[') {
            size_t section_length = text_length(content);
            if (section_length < 10 || content[section_length - 1] != ']')
                return fail(ParseError::InvalidSection);
            content[section_length - 1] = 0;
            char* spec = trim(content + 1);
            const char prefix[] = "service ";
            for (size_t i = 0; i < sizeof(prefix) - 1; ++i)
                if (spec[i] != prefix[i]) return fail(ParseError::InvalidSection);
            char* id = trim(spec + sizeof(prefix) - 1);
            if (!valid_id(id)) return fail(ParseError::InvalidIdentifier);
            ParseError completed = finish_section();
            if (completed != ParseError::None) return fail(completed);
            if (parsed.count == MaxServices) return fail(ParseError::CapacityExceeded);
            if (find_service(parsed, id) >= 0) return fail(ParseError::DuplicateService);
            initialize_definition(current);
            if (!copy_string(current.id, sizeof(current.id), id))
                return fail(ParseError::InvalidIdentifier);
            has_section = true;
            continue;
        }

        char* equals = content;
        while (*equals && *equals != '=') ++equals;
        if (!*equals) return fail(ParseError::InvalidValue);
        *equals++ = 0;
        char* key = trim(content);
        char* value = trim(equals);
        if (equal(key, "version")) {
            if (has_version || has_section) return fail(ParseError::InvalidValue);
            has_version = true;
            if (!equal(value, "1")) return fail(ParseError::UnsupportedVersion);
            continue;
        }
        if (!has_section) return fail(ParseError::InvalidSection);
        ParseError parsed_field = parse_field(current, key, value);
        if (parsed_field != ParseError::None) return fail(parsed_field);
    }

    if (!has_version) return fail(ParseError::MissingVersion);
    ParseError completed = finish_section();
    if (completed != ParseError::None) return fail(completed);
    if (!parsed.count) return fail(ParseError::InvalidInput);

    for (uint32_t i = 0; i < parsed.count; ++i) {
        ServiceDefinition& definition = parsed.services[i].definition;
        for (uint32_t j = 0; j < definition.dependency_count; ++j) {
            if (!definition.dependencies[j].optional &&
                find_service(parsed, definition.dependencies[j].service_id) < 0) {
                if (diagnostic) *diagnostic = {ParseError::MissingRequiredDependency, line_number};
                return false;
            }
        }
    }
    *output = parsed;
    return true;
}

PlanError build_startup_plan(const ServiceRegistry* registry, StartupPlan* plan) {
    if (!registry || !plan || registry->count > MaxServices) return PlanError::InvalidInput;
    plan->count = 0;
    uint32_t indegree[MaxServices] = {};
    bool emitted[MaxServices] = {};
    for (uint32_t i = 0; i < registry->count; ++i) {
        const ServiceDefinition& definition = registry->services[i].definition;
        if (definition.dependency_count > MaxDependencies) return PlanError::InvalidInput;
        for (uint32_t d = 0; d < definition.dependency_count; ++d) {
            int32_t dependency = find_service(*registry, definition.dependencies[d].service_id);
            if (dependency < 0) {
                if (!definition.dependencies[d].optional) return PlanError::MissingRequiredDependency;
                continue;
            }
            ++indegree[i];
        }
    }
    while (plan->count < registry->count) {
        int32_t selected = -1;
        for (uint32_t i = 0; i < registry->count; ++i)
            if (!emitted[i] && indegree[i] == 0) { selected = static_cast<int32_t>(i); break; }
        if (selected < 0) return PlanError::DependencyCycle;
        emitted[selected] = true;
        plan->service_indices[plan->count++] = static_cast<uint8_t>(selected);
        const char* completed_id = registry->services[selected].definition.id;
        for (uint32_t i = 0; i < registry->count; ++i) {
            if (emitted[i]) continue;
            const ServiceDefinition& definition = registry->services[i].definition;
            for (uint32_t d = 0; d < definition.dependency_count; ++d)
                if (equal(definition.dependencies[d].service_id, completed_id) && indegree[i])
                    --indegree[i];
        }
    }
    return PlanError::None;
}

DependencyStatus required_dependencies_status(const ServiceRegistry* registry,
                                             uint32_t service_index) {
    if (!registry || registry->count > MaxServices || service_index >= registry->count)
        return DependencyStatus::InvalidInput;

    const ServiceDefinition& definition = registry->services[service_index].definition;
    if (definition.dependency_count > MaxDependencies) return DependencyStatus::InvalidInput;
    for (uint32_t i = 0; i < definition.dependency_count; ++i) {
        const Dependency& dependency = definition.dependencies[i];
        if (dependency.optional) continue;

        int32_t required_index = find_service(*registry, dependency.service_id);
        if (required_index < 0) return DependencyStatus::RequiredDependencyFailed;

        const ServiceState state = registry->services[required_index].state;
        if (state == ServiceState::Failed || state == ServiceState::Stopped)
            return DependencyStatus::RequiredDependencyFailed;
        if (state != ServiceState::Running) return DependencyStatus::Waiting;
    }
    return DependencyStatus::Ready;
}

bool valid_transition(ServiceState from, ServiceState to) {
    switch (from) {
        case ServiceState::Registered:
            return to == ServiceState::WaitingForDependencies || to == ServiceState::Starting ||
                   to == ServiceState::Stopped || to == ServiceState::Failed;
        case ServiceState::WaitingForDependencies:
            return to == ServiceState::Starting || to == ServiceState::Failed || to == ServiceState::Stopped;
        case ServiceState::Starting:
            return to == ServiceState::Running || to == ServiceState::Failed || to == ServiceState::Stopping;
        case ServiceState::Running:
            return to == ServiceState::Stopping || to == ServiceState::Failed;
        case ServiceState::Stopping:
            return to == ServiceState::Stopped || to == ServiceState::Failed;
        case ServiceState::Stopped:
        case ServiceState::Failed:
            return to == ServiceState::Starting || to == ServiceState::Stopped;
    }
    return false;
}

bool transition(ServiceRecord* service, ServiceState next) {
    if (!service || !valid_transition(service->state, next)) return false;
    service->state = next;
    if (next == ServiceState::Stopping || next == ServiceState::Stopped)
        service->intentional_stop = true;
    else if (next == ServiceState::Starting)
        service->intentional_stop = false;
    return true;
}

bool restart_allowed(const ServiceRecord* service, int32_t status, bool intentional) {
    if (!service || intentional || service->intentional_stop ||
        service->restart_count >= service->definition.restart_limit) return false;
    switch (service->definition.restart_policy) {
        case RestartPolicy::Never: return false;
        case RestartPolicy::OnFailure: return status != 0;
        case RestartPolicy::Always: return true;
    }
    return false;
}

uint32_t restart_delay_ms(const ServiceDefinition* service, uint32_t restart_number) {
    if (!service || !restart_number) return 0;
    uint32_t delay = service->restart_backoff_ms;
    for (uint32_t i = 1; i < restart_number && delay < service->restart_backoff_max_ms; ++i) {
        if (delay > service->restart_backoff_max_ms / 2) return service->restart_backoff_max_ms;
        delay *= 2;
    }
    return delay > service->restart_backoff_max_ms ? service->restart_backoff_max_ms : delay;
}

} // namespace DrakOS::SSM
