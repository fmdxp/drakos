.global _start
.extern main
.extern exit

.text
_start:
    xor %rbp, %rbp
    mov (%rsp), %rdi                 # argc
    lea 8(%rsp), %rsi                # argv
    lea 16(%rsp,%rdi,8), %rdx        # envp after argv[argc] == NULL
    and $-16, %rsp
    call main
    mov %eax, %edi
    call exit
1:
    hlt
    jmp 1b
