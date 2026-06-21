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

// GDT Entry structure (8 bytes)
struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t flags; // High 4 bits (flags) + Low 4 bits (limit_high)
    uint8_t base_high;
} __attribute__((packed));

// GDT Pointer structure (10 bytes) used by lgdt
struct GDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// TSS Entry structure (104 bytes in 64-bit mode)
struct TSSEntry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

class GDT : public KernelModule {
public:
    bool start() override;
    void stop() override;
    const char* get_name() const override;
    
    // Updates RSP0 in the TSS for the current thread
    void set_kernel_stack(uintptr_t stack);

private:
    void set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags);
    void set_tss(int index, uintptr_t base, uint32_t limit);
    
    // We need 7 entries: Null, KCode, KData, UData, UCode, TSS (2 entries)
    GDTEntry entries[7];
    GDTPointer pointer;
    TSSEntry tss;
};
