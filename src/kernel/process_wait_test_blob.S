.section .rodata
.global process_wait_blob_start
.global process_wait_blob_end
.global process_wait_specific
.global process_wait_any
.global process_wait_child_worker
.global process_wait_multi_parent
.global process_wait_child_worker2
.global process_wait_orphan_parent

process_wait_blob_start:
process_wait_specific:
    movabs $0x401000, %rax
    movq (%rax), %rbx
    movq %rbx, %rdi
    movabs $0x0000800000000000, %rsi
    movl $61, %eax
    syscall
    cmpq $-14, %rax
    je process_wait_specific_validated
    movabs $0x401070, %rcx
    lock incq (%rcx)
process_wait_specific_validated:
    movq %rbx, %rdi
    jmp process_wait_common

process_wait_any:
    movabs $0x401000, %rax
    movq (%rax), %rbx
    movq $-1, %rdi

process_wait_common:
    movabs $0x401008, %rsi
    movl $61, %eax
    syscall
    cmpq %rbx, %rax
    jne process_wait_check_echild
    movabs $0x401008, %rcx
    cmpl $23, (%rcx)
    jne process_wait_error
    movabs $0x401018, %rcx
    lock incq (%rcx)
    jmp process_wait_release_gate

process_wait_check_echild:
    cmpq $-10, %rax
    jne process_wait_error
    movabs $0x401020, %rcx
    lock incq (%rcx)
    jmp process_wait_release_gate

process_wait_error:
    movabs $0x401028, %rcx
    lock incq (%rcx)

process_wait_release_gate:
    movabs $0x401030, %rcx
    cmpq $0, (%rcx)
    jne process_wait_exit
    movl $24, %eax
    syscall
    jmp process_wait_release_gate

process_wait_exit:
    xorl %edi, %edi
    movl $60, %eax
    syscall
    ud2

.org 0x100
process_wait_child_worker:
    movabs $0x401060, %rcx
    movq $1, (%rcx)
    movabs $0x401010, %rcx
    cmpq $0, (%rcx)
    jne process_wait_child_exit
    movl $24, %eax
    syscall
    jmp process_wait_child_worker
process_wait_child_exit:
    movl $23, %edi
    movl $60, %eax
    syscall
    ud2
process_wait_blob_end:

.org 0x200
process_wait_multi_parent:
    movabs $0x401008, %rsi
    movq $-1, %rdi
    movl $61, %eax
    syscall
    movabs $0x401050, %rcx
    movq %rax, (%rcx)
    testq %rax, %rax
    js process_wait_multi_error
    movabs $0x401018, %rcx
    lock incq (%rcx)

    movabs $0x401040, %rsi
    movq $-1, %rdi
    movl $61, %eax
    syscall
    movabs $0x401058, %rcx
    movq %rax, (%rcx)
    testq %rax, %rax
    js process_wait_multi_error
    movabs $0x401018, %rcx
    lock incq (%rcx)
    jmp process_wait_multi_release

process_wait_multi_error:
    movabs $0x401028, %rcx
    lock incq (%rcx)

process_wait_multi_release:
    movabs $0x401030, %rcx
    cmpq $0, (%rcx)
    jne process_wait_multi_exit
    movl $24, %eax
    syscall
    jmp process_wait_multi_release

process_wait_multi_exit:
    xorl %edi, %edi
    movl $60, %eax
    syscall
    ud2

.org 0x300
process_wait_child_worker2:
    movabs $0x401068, %rcx
    movq $1, (%rcx)
    movabs $0x401010, %rcx
    cmpq $0, (%rcx)
    jne process_wait_child_exit2
    movl $24, %eax
    syscall
    jmp process_wait_child_worker2
process_wait_child_exit2:
    movl $47, %edi
    movl $60, %eax
    syscall
    ud2
.global process_wait_blob_final_end
.org 0x400
process_wait_orphan_parent:
    xorl %edi, %edi
    movl $60, %eax
    syscall
    ud2

process_wait_blob_final_end:
