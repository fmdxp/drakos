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

// Enable AVX/AVX512 and XSAVE support dynamically based on CPUID
void enable_fpu_and_avx() {
    uint32_t eax, ebx, ecx, edx;

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return;

    // enable SSE base
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);   // OSFXSR
    cr4 |= (1 << 10);  // OSXMMEXCPT
    cr4 |= (1 << 18);  // OSXSAVE
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    // check XSAVE + AVX capability
    bool has_xsave = ecx & (1 << 26);
    bool has_avx   = ecx & (1 << 28);

    uint64_t xcr0 = (1 << 0) | (1 << 1); // x87 + SSE

    if (has_xsave) {
        if (has_avx) xcr0 |= (1 << 2);

        if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) &&
            (ebx & (1 << 16))) {
            xcr0 |= (1ULL << 5) | (1ULL << 6) | (1ULL << 7);
        }

        asm volatile("xsetbv"
            :
            : "a"((uint32_t)xcr0),
              "d"((uint32_t)(xcr0 >> 32)),
              "c"(0)
        );
    }
}
