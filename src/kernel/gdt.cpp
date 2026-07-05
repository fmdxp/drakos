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
    set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);

    // 2: Kernel Data Segment (DS=0x10)
    set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);

    // 3: User Data Segment (DS=0x18 - typically placed before User Code for SYSRET)
    set_entry(3, 0, 0xFFFFF, 0xF2, 0xC0);

    // 4: User Code Segment (CS=0x20)
    set_entry(4, 0, 0xFFFFF, 0xFA, 0xA0);
    
    // Clear TSS
    for (size_t i = 0; i < sizeof(TSSEntry); i++) {
        ((uint8_t*)&tss)[i] = 0;
    }
    tss.iopb_offset = sizeof(TSSEntry); // No IOPB
    
    // 5 & 6: TSS Segment (CS=0x28)
    set_tss(5, (uintptr_t)&tss, sizeof(TSSEntry) - 1);

    // Setup pointer
    pointer.limit = (sizeof(GDTEntry) * 7) - 1;
    pointer.base = (uint64_t)&entries;

    // Load GDT
    asm volatile("lgdt %0" : : "m"(pointer));

    // Reload segment registers with the new GDT selectors
    reload_segments();
    
    // Load TSS (Selector 0x28, which is index 5 * 8)
    asm volatile("ltr %0" : : "r"((uint16_t)0x28));

    return true;
}

void GDT::stop() {
    // Cannot really stop a GDT once active, unless reverting to a previous one.
}

const char* GDT::get_name() const {
    return "Global Descriptor Table (GDT)";
}

void GDT::set_kernel_stack(uintptr_t stack) {
    tss.rsp0 = stack;
}

void GDT::set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    entries[index].base_low = (base & 0xFFFF);
    entries[index].base_middle = (base >> 16) & 0xFF;
    entries[index].base_high = (base >> 24) & 0xFF;

    entries[index].limit_low = (limit & 0xFFFF);
    entries[index].flags = ((limit >> 16) & 0x0F) | (flags & 0xF0);
    entries[index].access = access;
}

void GDT::set_tss(int index, uintptr_t base, uint32_t limit) {
    set_entry(index, base, limit, 0x89, 0x00); // Access 0x89: Present, TSS Available
    
    // In 64-bit mode, the TSS descriptor is 16 bytes (2 consecutive GDT entries)
    // The second entry contains the upper 32 bits of the base address.
    entries[index + 1].limit_low = (base >> 32) & 0xFFFF;
    entries[index + 1].base_low = (base >> 48) & 0xFFFF;
    entries[index + 1].base_middle = 0;
    entries[index + 1].access = 0;
    entries[index + 1].flags = 0;
    entries[index + 1].base_high = 0;
}

// Register as Core Module
REGISTER_MODULE(g_gdt, GDT, 1_core);
