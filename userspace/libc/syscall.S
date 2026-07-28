.global syscall0
.global syscall1
.global syscall2
.global syscall3
.global syscall4
.global syscall5
.global syscall6

.text

// uint64_t syscall0(uint64_t num);
syscall0:
    mov %rdi, %rax
    syscall
    ret

// uint64_t syscall1(uint64_t num, uint64_t arg1);
syscall1:
    mov %rdi, %rax
    mov %rsi, %rdi
    syscall
    ret

// uint64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2);
syscall2:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    syscall
    ret

// uint64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3);
syscall3:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    syscall
    ret

// uint64_t syscall4(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);
syscall4:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    syscall
    ret

// uint64_t syscall5(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
syscall5:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    syscall
    ret

// uint64_t syscall6(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);
syscall6:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    mov 8(%rsp), %r9
    syscall
    ret
