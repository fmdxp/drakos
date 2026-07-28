#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t syscall0(uint64_t num);
uint64_t syscall1(uint64_t num, uint64_t arg1);
uint64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2);
uint64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3);
uint64_t syscall4(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);
uint64_t syscall5(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
uint64_t syscall6(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6);

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_stat       4
#define SYS_fstat      5
#define SYS_lseek      8
#define SYS_mmap       9
#define SYS_mprotect   10
#define SYS_munmap     11
#define SYS_brk        12
#define SYS_access     21
#define SYS_sched_yield 24
#define SYS_dup        32
#define SYS_dup2       33
#define SYS_nanosleep  35
#define SYS_getpid     39
#define SYS_exit       60
#define SYS_uname      63
#define SYS_fcntl      72
#define SYS_getdents   78
#define SYS_getcwd     79
#define SYS_chdir      80
#define SYS_mkdir      83
#define SYS_rmdir      84
#define SYS_umask      95
#define SYS_gettimeofday 96
#define SYS_getuid     102

#ifdef __cplusplus
}
#endif
