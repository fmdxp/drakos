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

.global isr_common
.extern isr_handler
.extern scheduler_switch

isr_common:
    # Save all general purpose registers
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    # The parameter for isr_handler (Context*) is RSP
    mov %rsp, %rdi
    call isr_handler

    # If we are about to return and there is a pending schedule, scheduler_switch
    # receives the current Context* (RSP) and returns the new Context* (in RAX)
    mov %rsp, %rdi
    call scheduler_switch
    mov %rax, %rsp # Update RSP with the new or the same context

    # Restore all general purpose registers
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    # Skip int_no and err_code (16 bytes)
    add $16, %rsp
    iretq

# Generation of stubs for the 256 interrupts (from 0 to 255)
.macro ISR_NOERR num
.global isr_\num
isr_\num:
    push $0 # Dummy error code
    push $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr_\num
isr_\num:
    # The hardware has already pushed the error code
    push $\num
    jmp isr_common
.endm

# Exceptions
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR 21

.altmacro
.set i, 22
.rept 234
    ISR_NOERR %i
    .set i, i+1
.endr

.global syscall_entry
.extern syscall_handler

syscall_entry:

    push %rcx
    push %r11
    
    push %rdi
    push %rsi
    push %rdx
    push %r10
    push %r8
    push %r9
    push %rax
    
    mov %rsp, %rdi
    call syscall_handler
    
    pop %rax
    pop %r9
    pop %r8
    pop %r10
    pop %rdx
    pop %rsi
    pop %rdi
    
    pop %r11
    pop %rcx
    
    sysretq
