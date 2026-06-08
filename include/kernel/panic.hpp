#pragma once

#include "idt.hpp"

// Kernel Panic function
// Halts the system and displays an error message on a red background.
[[noreturn]] void panic(const char* message, InterruptFrame* frame = nullptr);
