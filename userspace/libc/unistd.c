#include "unistd.h"
#include "syscall.h"

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall3(SYS_read, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall3(SYS_write, (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int open(const char *pathname, int flags) {
    return (int)syscall2(SYS_open, (uint64_t)pathname, (uint64_t)flags);
}

int close(int fd) {
    return (int)syscall1(SYS_close, (uint64_t)fd);
}

int lseek(int fd, int offset, int whence) {
    return (int)syscall3(SYS_lseek, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence);
}

int getpid(void) {
    return (int)syscall0(SYS_getpid);
}

void sched_yield(void) {
    syscall0(SYS_sched_yield);
}

unsigned int sleep(unsigned int seconds) {
    uint64_t req[2] = {seconds, 0};
    syscall1(SYS_nanosleep, (uint64_t)req);
    return 0;
}

int usleep(unsigned int usec) {
    uint64_t req[2] = {usec / 1000000, (usec % 1000000) * 1000};
    syscall1(SYS_nanosleep, (uint64_t)req);
    return 0;
}
