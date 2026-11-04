/* ClaudeOS user-space API: system calls + a tiny C library.
 * Include this in your program; see README "Adding your own program". */
#ifndef CLAUDEOS_H
#define CLAUDEOS_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "../../common/format.h"

/* --- system calls (int 0x80) --- */
void  exit(int code) __attribute__((noreturn));
int   write(int fd, const void *buf, size_t len);   /* fd 1 = terminal */
int   read(int fd, void *buf, size_t len);          /* fd 0 = line from keyboard */
int   open(const char *path);                       /* read-only, returns fd >= 3 */
int   close(int fd);
int   getpid(void);
void  sleep_ms(unsigned ms);
unsigned ticks_ms(void);                            /* milliseconds since boot */
void *sbrk(int increment);
void  yield(void);
int   getkey(void);                                 /* non-blocking, -1 if none */
int   stat_size(const char *path);                  /* file size or <0 */

/* --- libc subset --- */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *d, const char *s);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
int    atoi(const char *s);
int    printf(const char *fmt, ...);
int    puts(const char *s);
int    putchar(int c);
char  *gets_s(char *buf, size_t size);              /* reads a line, strips '\n' */
void  *malloc(size_t n);
void   free(void *p);
unsigned rand(void);
void   srand(unsigned seed);
#endif
