#pragma once
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char* format, ...);
int puts(const char* s);
int putchar(int c);

#ifdef __cplusplus
}
#endif
