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

// Enable FPU/SSE/AVX support dynamically based on CPUID.
// NOTE: We deliberately limit XCR0 to x87 + SSE + AVX (bits 0,1,2).
// AVX-512 (bits 5,6,7) and AMX (bits 17,18) would push the XSAVE area
// well beyond our FPU_STATE_SIZE (4096 bytes) inside each Thread struct,
// corrupting adjacent memory. Limit to 832 bytes max (512 legacy + 64 header + 256 AVX).
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
