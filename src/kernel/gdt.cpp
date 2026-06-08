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


#include "gdt.hpp"

// External assembly function to reload segment registers after GDT update
// We will write it using inline assembly here to keep it pure C++
static void reload_segments() {
    // Reload CS register using a far return trick (retfq).
    // We use a numeric local label (1:) to avoid symbol conflicts
    // when linking multiple translation units.
    asm volatile (
        "pushq %0\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "retfq\n"
        "1:\n"
        "mov %1, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : 
        : "i"(0x08), "i"(0x10) // 0x08 = Kernel Code, 0x10 = Kernel Data
        : "rax", "memory"
    );
}

bool GDT::start() {
    // 0: Null Entry
    set_entry(0, 0, 0, 0, 0);

    // 1: Kernel Code Segment (CS=0x08)
    // Access: Present(1) | Privilege(00) | Descriptor(1) | Executable(1) | Conforming(0) | Readable(1) | Accessed(0) = 0x9A
    // Flags: Granularity(1) | Size(0 - 64bit uses L) | LongMode(1) | Available(0) = 0xA0
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);

    // 2: Kernel Data Segment (DS=0x10)
    // Access: Present(1) | Privilege(00) | Descriptor(1) | Executable(0) | Direction(0) | Writable(1) | Accessed(0) = 0x92
    // Flags: Granularity(1) | Size(1) | LongMode(0) | Available(0) = 0xC0
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);

    // 3: User Data Segment (DS=0x18 - typically placed before User Code for SYSRET)
    // Access: Present(1) | Privilege(11) | Descriptor(1) | Executable(0) | Direction(0) | Writable(1) | Accessed(0) = 0xF2
    set_entry(3, 0, 0xFFFFF, 0xF2, 0xC0);

    // 4: User Code Segment (CS=0x20)
    // Access: Present(1) | Privilege(11) | Descriptor(1) | Executable(1) | Conforming(0) | Readable(1) | Accessed(0) = 0xFA
    set_entry(4, 0, 0xFFFFF, 0xFA, 0xA0);

    // Setup pointer
    pointer.limit = (sizeof(GDTEntry) * 5) - 1;
    pointer.base = (uint64_t)&entries;

    // Load GDT
    asm volatile("lgdt %0" : : "m"(pointer));

    // Reload segment registers with the new GDT selectors
    reload_segments();

    return true;
}

void GDT::stop() {
    // Cannot really stop a GDT once active, unless reverting to a previous one.
}

const char* GDT::get_name() const {
    return "Global Descriptor Table (GDT)";
}

void GDT::set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    entries[index].base_low = (base & 0xFFFF);
    entries[index].base_middle = (base >> 16) & 0xFF;
    entries[index].base_high = (base >> 24) & 0xFF;

    entries[index].limit_low = (limit & 0xFFFF);
    entries[index].flags = ((limit >> 16) & 0x0F) | (flags & 0xF0);
    entries[index].access = access;
}

// Register as Core Module
REGISTER_MODULE(g_gdt, GDT, 1_core);
