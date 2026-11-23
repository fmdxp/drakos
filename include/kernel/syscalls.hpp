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


struct SyscallFrame {
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t num;       // also used as return value slot
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rcx;       // user RIP
    uint64_t r11;       // user RFLAGS
};

#define EPERM    1
#define ENOENT   2
#define EBADF    9
#define ECHILD   10
#define EAGAIN   11
#define ENOMEM   12
#define EFAULT   14
#define EINVAL   22
#define ESPIPE   29
#define EMFILE   24
#define ENOSYS   38

extern "C" void syscall_entry();
void enable_syscalls();
extern "C" void syscall_handler(SyscallFrame* frame);
