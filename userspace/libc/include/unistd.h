#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t ssize_t;

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int open(const char *pathname, int flags);
int close(int fd);
int lseek(int fd, int offset, int whence);
int getpid(void);
void sched_yield(void);

unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);

#ifdef __cplusplus
}
#endif
