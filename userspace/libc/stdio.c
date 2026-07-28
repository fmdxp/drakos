#include "stdio.h"
#include "unistd.h"
#include "string.h"
#include <stdarg.h>

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char* s) {
    write(1, s, strlen(s));
    char newline = '\n';
    write(1, &newline, 1);
    return 0;
}

// Very basic printf for testing
int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    while (*format) {
        if (*format == '%') {
            format++;
            if (*format == 's') {
                const char* s = va_arg(args, const char*);
                write(1, s, strlen(s));
            } else if (*format == 'd') {
                int val = va_arg(args, int);
                if (val == 0) {
                    putchar('0');
                } else {
                    char buf[16];
                    int i = 0;
                    if (val < 0) {
                        putchar('-');
                        val = -val;
                    }
                    while (val > 0) {
                        buf[i++] = '0' + (val % 10);
                        val /= 10;
                    }
                    while (i > 0) {
                        putchar(buf[--i]);
                    }
                }
            } else if (*format == 'x' || *format == 'p' || (*format == 'l' && *(format+1) == 'x')) {
                int is_long = 0;
                if (*format == 'p') {
                    putchar('0'); putchar('x');
                    is_long = 1;
                } else if (*format == 'l') {
                    is_long = 1;
                    format++; // skip 'l'
                }
                
                uint64_t val;
                if (is_long) val = va_arg(args, uint64_t);
                else val = va_arg(args, unsigned int);

                if (val == 0) {
                    putchar('0');
                } else {
                    char buf[20];
                    int i = 0;
                    while (val > 0) {
                        int rem = val % 16;
                        if (rem < 10) buf[i++] = '0' + rem;
                        else buf[i++] = 'a' + (rem - 10);
                        val /= 16;
                    }
                    while (i > 0) {
                        putchar(buf[--i]);
                    }
                }
            } else {
                putchar(*format);
            }
        } else {
            putchar(*format);
        }
        format++;
    }

    va_end(args);
    return 0;
}
