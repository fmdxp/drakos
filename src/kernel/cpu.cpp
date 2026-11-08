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


#include "cpu.hpp"

#include <stdint.h>
#include <cpuid.h>

static bool s_nx_enabled = false;

bool nx_enabled() { return s_nx_enabled; }

bool enable_nx() {
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_leaf = __get_cpuid_max(0x80000000, nullptr);
    if (max_leaf < 0x80000001 || !__get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) ||
        !(edx & (1u << 20))) {
        s_nx_enabled = false;
        return false;
    }
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    uint64_t efer = (static_cast<uint64_t>(high) << 32) | low;
    efer |= 1ULL << 11; // EFER.NXE
    low = static_cast<uint32_t>(efer);
    high = static_cast<uint32_t>(efer >> 32);
    asm volatile("wrmsr" :: "c"(0xC0000080), "a"(low), "d"(high) : "memory");
    s_nx_enabled = true;
    return true;
}

// Enable FPU/SSE/AVX support dynamically based on CPUID.
// NOTE: We deliberately limit XCR0 to x87 + SSE + AVX (bits 0,1,2).
// AVX-512 (bits 5,6,7) and AMX (bits 17,18) would push the XSAVE area
// well beyond our FPU_STATE_SIZE (4096 bytes) inside each Thread struct,
// corrupting adjacent memory. Limit to 832 bytes max (512 legacy + 64 header + 256 AVX).
// This is available from all CPU from 2011 and up
void enable_fpu_and_avx() {
    uint32_t eax, ebx, ecx, edx;

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return;

    // Enable SSE base (OSFXSR, OSXMMEXCPT)
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);   // OSFXSR
    cr4 |= (1 << 10);  // OSXMMEXCPT
    cr4 |= (1 << 18);  // OSXSAVE (needed for xsave64/xrstor64)
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    bool has_xsave = ecx & (1 << 26);
    bool has_avx   = ecx & (1 << 28);

    if (has_xsave) {
        // ONLY enable: bit 0 (x87), bit 1 (SSE), bit 2 (AVX if available)
        // Do NOT enable AVX-512 (bits 5,6,7), AMX (bits 17,18) or anything else
        // as their XSAVE components would exceed our 4096-byte fpu_state buffer.
        uint64_t xcr0 = (1ULL << 0) | (1ULL << 1); // x87 + SSE always
        if (has_avx) xcr0 |= (1ULL << 2);           // AVX optional

        asm volatile("xsetbv"
            :
            : "a"((uint32_t)xcr0),
              "d"((uint32_t)(xcr0 >> 32)),
              "c"(0)
        );
    }
}
