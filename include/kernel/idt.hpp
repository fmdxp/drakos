/*
 * drakos - An x64 UEFI gaming OS inspired by the architecture and user experience of modern consoles.
 * Copyright (C) 2026 fmdxp
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#pragma once

#include <stdint.h>
#include "module.hpp"

// IDT Entry for x86_64 (16 bytes)
struct IDTEntry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t ist;        // Interrupt Stack Table offset
    uint8_t attributes; // Type and attributes
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed));

// IDT Pointer structure (10 bytes) used by lidt
struct IDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// Interrupt frame passed by GCC when using __attribute__((interrupt))
struct InterruptFrame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

class IDT : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;

private:
    void set_entry(int index, uint64_t isr, uint8_t flags);
    
    // We need 256 entries for a complete IDT
    IDTEntry entries[256];
    IDTPointer pointer;
};
