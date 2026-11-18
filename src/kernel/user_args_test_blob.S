.section .text.user_args_test,"ax"
.global user_args_test_start
.global user_args_test_success_fault
.global user_args_test_end
.type user_args_test_start, @object
user_args_test_start:
    movq %rsp, %rax                 # Keep RSP for the kernel marker.
    cmpq $2, (%rsp)
    jne user_args_test_fail
    movq 8(%rsp), %rcx
    testq %rcx, %rcx
    jz user_args_test_fail
    cmpb $'a', (%rcx)
    jne user_args_test_fail
    movq 16(%rsp), %rcx
    testq %rcx, %rcx
    jz user_args_test_fail
    cmpb $'b', (%rcx)
    jne user_args_test_fail
    cmpq $0, 24(%rsp)
    jne user_args_test_fail
    movq 32(%rsp), %rcx
    testq %rcx, %rcx
    jz user_args_test_fail
    cmpb $'M', (%rcx)
    jne user_args_test_fail
    cmpq $0, 40(%rsp)
    jne user_args_test_fail
    movabsq $0x500000, %rbx
    movq %rax, (%rbx)
    movl $39, %eax
    syscall
    movq %rax, 8(%rbx)
user_args_test_success_fault:
    ud2
user_args_test_fail:
    ud2
user_args_test_end:
.size user_args_test_start, user_args_test_end-user_args_test_start

.section .data
.global user_args_test_marker
user_args_test_marker:
    .zero 16
