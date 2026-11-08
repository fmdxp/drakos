#pragma once

#include <stddef.h>
#include <stdint.h>

enum class BootMode : uint8_t {
    Normal,
    Verbose,
    Recovery,
};

void boot_diagnostics_init(const char* command_line);
BootMode boot_diagnostics_mode();
bool boot_diagnostics_visible();
void boot_diagnostics_capture(const char* text);
void boot_diagnostics_write(const char* text);
void boot_diagnostics_write_force(const char* text);
void boot_diagnostics_dump();
