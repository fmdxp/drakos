.section .text.process_exit_test,"ax"
.global process_exit_worker_start
.global process_exit_worker_end
.global process_exit_test_end
.global process_exit_wait_37
.global process_exit_wait_41
.global process_exit_zero

process_exit_worker_start:
    movabsq $0x401000, %rbx
1:
    lock incq (%rbx)
    jmp 1b
process_exit_worker_end:

process_exit_wait_37:
    movabsq $0x401008, %rbx
2:
    cmpq $0, (%rbx)
    je 2b
    movl $37, %edi
    movl $60, %eax
    syscall
    ud2

process_exit_wait_41:
    movabsq $0x401008, %rbx
3:
    cmpq $0, (%rbx)
    je 3b
    movl $41, %edi
    movl $60, %eax
    syscall
    ud2

process_exit_zero:
    xorl %edi, %edi
    movl $60, %eax
    syscall
    ud2
process_exit_test_end:
