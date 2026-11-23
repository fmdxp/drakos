.section .rodata
.global process_termination_blob_start
.global process_termination_blob_end
.global process_termination_parent
.global process_termination_child_a
.global process_termination_child_b

process_termination_blob_start:
.org 0x500
process_termination_parent:
    movabs $0x401000, %rax
    movq (%rax), %rbx

process_termination_wait_ready:
    movabs $0x401060, %rcx
    cmpq $1, (%rcx)
    jne process_termination_yield
    movabs $0x401068, %rcx
    cmpq $1, (%rcx)
    je process_termination_validate
process_termination_yield:
    movl $24, %eax
    syscall
    jmp process_termination_wait_ready

process_termination_validate:
    movabs $0x401010, %rax
    movq (%rax), %r12
    movq %r12, %rdi
    movabs $0x401028, %rsi
    movl $61, %eax
    syscall
    cmpq %r12, %rax
    jne process_termination_fail
    movabs $0x401028, %rcx
    cmpl $37, (%rcx)
    jne process_termination_fail
    movabs $0x401088, %rcx
    lock incq (%rcx)

    xorl %edi, %edi
    movl $77, %esi
    movl $62, %eax
    syscall
    cmpq $-22, %rax
    jne process_termination_fail

    movabs $0x401038, %rax
    movq (%rax), %rdi
    movl $77, %esi
    movl $62, %eax
    syscall
    cmpq $-3, %rax
    jne process_termination_fail

    movq %rbx, %rdi
    movl $91, %esi
    movl $62, %eax
    syscall
    testq %rax, %rax
    jne process_termination_fail

    movq %rbx, %rdi
    movabs $0x401008, %rsi
    movl $61, %eax
    syscall
    cmpq %rbx, %rax
    jne process_termination_fail
    movabs $0x401008, %rcx
    cmpl $91, (%rcx)
    jne process_termination_fail
    movabs $0x401078, %rcx
    lock incq (%rcx)
    xorl %edi, %edi
    jmp process_termination_exit

process_termination_fail:
    movabs $0x401080, %rcx
    lock incq (%rcx)
    movl $1, %edi
process_termination_exit:
    movl $60, %eax
    syscall
    ud2

.org 0x700
process_termination_child_a:
    movabs $0x401060, %rcx
    movq $1, (%rcx)
    movabs $0x401070, %rcx
process_termination_worker_a_loop:
    lock incq (%rcx)
    jmp process_termination_worker_a_loop

.org 0x780
process_termination_child_b:
    movabs $0x401068, %rcx
    movq $1, (%rcx)
    movabs $0x401070, %rcx
process_termination_worker_b_loop:
    lock incq (%rcx)
    jmp process_termination_worker_b_loop

process_termination_blob_end:
