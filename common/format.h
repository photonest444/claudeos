/* vsnprintf shared by the kernel and the user-space C library. */
#ifndef COMMON_FORMAT_H
#define COMMON_FORMAT_H
#include <stdarg.h>
#include <stddef.h>

/* Supports %d %i %u %x %X %p %s %c %% with optional '-', '0', width and 'l'. */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
#endif
