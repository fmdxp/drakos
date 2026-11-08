#include "kernel/boot_diagnostics.hpp"

#include "drivers/serial.hpp"
#include "drivers/vga.hpp"

static constexpr size_t LOG_CAPACITY = 8192;
static char s_log[LOG_CAPACITY];
static size_t s_log_next = 0;
static size_t s_log_size = 0;
static BootMode s_mode = BootMode::Normal;

static bool token_present(const char* text, const char* token) {
    if (!text || !token) return false;
    for (const char* p = text; *p; ++p) {
        if (p != text && p[-1] != ' ' && p[-1] != '\t') continue;
        const char* a = p;
        const char* b = token;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*b == '\0' && (*a == '\0' || *a == ' ' || *a == '\t')) return true;
    }
    return false;
}

void boot_diagnostics_init(const char* command_line) {
    s_mode = BootMode::Normal;
    if (token_present(command_line, "drakos.mode=verbose")) s_mode = BootMode::Verbose;
    else if (token_present(command_line, "drakos.mode=recovery")) s_mode = BootMode::Recovery;
}

BootMode boot_diagnostics_mode() { return s_mode; }
bool boot_diagnostics_visible() { return s_mode != BootMode::Normal; }

void boot_diagnostics_capture(const char* text) {
    if (!text) return;
    while (*text) {
        s_log[s_log_next] = *text++;
        s_log_next = (s_log_next + 1) % LOG_CAPACITY;
        if (s_log_size < LOG_CAPACITY) ++s_log_size;
    }
}

static void raw_write(const char* text) {
    if (!text) return;
    if (g_vga) g_vga->write_raw(text);
    else if (g_serial) g_serial->write_raw(text);
}

void boot_diagnostics_write(const char* text) {
    boot_diagnostics_capture(text);
    if (boot_diagnostics_visible()) raw_write(text);
}

void boot_diagnostics_write_force(const char* text) {
    boot_diagnostics_capture(text);
    raw_write(text);
}

void boot_diagnostics_dump() {
    raw_write("\n--- captured boot diagnostics ---\n");
    size_t first = (s_log_next + LOG_CAPACITY - s_log_size) % LOG_CAPACITY;
    char chunk[65];
    size_t used = 0;
    for (size_t i = 0; i < s_log_size; ++i) {
        chunk[used++] = s_log[(first + i) % LOG_CAPACITY];
        if (used == sizeof(chunk) - 1) {
            chunk[used] = '\0';
            raw_write(chunk);
            used = 0;
        }
    }
    if (used) {
        chunk[used] = '\0';
        raw_write(chunk);
    }
    raw_write("\n--- end diagnostics ---\n");
}
