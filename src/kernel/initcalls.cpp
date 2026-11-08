#include "module.hpp"
#include "kernel/boot_diagnostics.hpp"

extern "C" const InitcallDescriptor __initcalls_start[];
extern "C" const InitcallDescriptor __initcalls_end[];

static InitcallRecord s_records[INITCALL_MAX_COUNT];

static uint64_t read_tsc() {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<uint64_t>(high) << 32) | low;
}

static void append_text(char* out, size_t& len, const char* text, size_t capacity) {
    if (!text) return;
    while (*text && len + 1 < capacity) out[len++] = *text++;
    out[len] = '\0';
}

static void append_uint(char* out, size_t& len, uint64_t value, size_t capacity) {
    char digits[24];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count && len + 1 < capacity) out[len++] = digits[--count];
    out[len] = '\0';
}

static void report_initcall(const InitcallRecord& record) {
    char line[256] = {};
    size_t len = 0;
    append_text(line, len, "initcall ", sizeof(line));
    append_text(line, len, record.descriptor->name, sizeof(line));
    append_text(line, len, ": ", sizeof(line));
    append_text(line, len, initcall_status_name(record.status), sizeof(line));
    append_text(line, len, " (", sizeof(line));
    append_uint(line, len, record.duration_ticks, sizeof(line));
    append_text(line, len, " TSC ticks)", sizeof(line));
    if (record.error) {
        append_text(line, len, " — ", sizeof(line));
        append_text(line, len, record.error, sizeof(line));
    }
    append_text(line, len, "\n", sizeof(line));
    boot_diagnostics_write(line);
}

InitcallSummary system_init_modules() {
    size_t count = static_cast<size_t>(__initcalls_end - __initcalls_start);
    InitcallSummary summary = initcall_run_all(__initcalls_start, count, s_records,
                                                INITCALL_MAX_COUNT, read_tsc);

    boot_diagnostics_write("drakos boot diagnostics\n");
    for (size_t i = 0; i < summary.count; ++i) report_initcall(s_records[i]);

    char totals[128] = {};
    size_t len = 0;
    append_text(totals, len, "initcalls: ", sizeof(totals));
    append_uint(totals, len, summary.succeeded, sizeof(totals));
    append_text(totals, len, " succeeded, ", sizeof(totals));
    append_uint(totals, len, summary.optional_failures, sizeof(totals));
    append_text(totals, len, " optional failures, ", sizeof(totals));
    append_uint(totals, len, summary.mandatory_failures, sizeof(totals));
    append_text(totals, len, " mandatory failures\n", sizeof(totals));
    boot_diagnostics_write(totals);

    return summary;
}
