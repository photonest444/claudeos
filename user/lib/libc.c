/* Minimal C library for ClaudeOS user programs. */
#include "claudeos.h"

enum {
    SYS_EXIT = 0, SYS_WRITE, SYS_READ, SYS_OPEN, SYS_CLOSE, SYS_GETPID,
    SYS_SLEEP, SYS_TICKS, SYS_SBRK, SYS_YIELD, SYS_GETKEY, SYS_STAT,
};

static inline int syscall3(int n, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

void exit(int code) { syscall3(SYS_EXIT, code, 0, 0); for (;;) ; }
int write(int fd, const void *buf, size_t len) { return syscall3(SYS_WRITE, fd, (int)buf, (int)len); }
int read(int fd, void *buf, size_t len) { return syscall3(SYS_READ, fd, (int)buf, (int)len); }
int open(const char *path) { return syscall3(SYS_OPEN, (int)path, 0, 0); }
int close(int fd) { return syscall3(SYS_CLOSE, fd, 0, 0); }
int getpid(void) { return syscall3(SYS_GETPID, 0, 0, 0); }
void sleep_ms(unsigned ms) { syscall3(SYS_SLEEP, (int)ms, 0, 0); }
unsigned ticks_ms(void) { return (unsigned)syscall3(SYS_TICKS, 0, 0, 0); }
void *sbrk(int inc) { return (void *)syscall3(SYS_SBRK, inc, 0, 0); }
void yield(void) { syscall3(SYS_YIELD, 0, 0, 0); }
int getkey(void) { return syscall3(SYS_GETKEY, 0, 0, 0); }
int stat_size(const char *path) { return syscall3(SYS_STAT, (int)path, 0, 0); }

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

int atoi(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    return write(1, buf, n);
}

int puts(const char *s) { write(1, s, strlen(s)); return write(1, "\n", 1); }
int putchar(int c) { char ch = (char)c; write(1, &ch, 1); return c; }

char *gets_s(char *buf, size_t size) {
    int n = read(0, buf, size - 1);
    if (n <= 0) return NULL;
    buf[n] = 0;
    if (buf[n - 1] == '\n') buf[n - 1] = 0;
    return buf;
}

/* Allocator: free-list of blocks carved from sbrk(). */
typedef struct block { size_t size; struct block *next; } block_t;
static block_t *free_list;

void *malloc(size_t n) {
    n = (n + 15) & ~15u;
    for (block_t **pp = &free_list; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= n) { block_t *b = *pp; *pp = b->next; return b + 1; }
    block_t *b = sbrk(n + sizeof(block_t));
    if (b == (void *)-1) return NULL;
    b->size = n;
    return b + 1;
}

void free(void *p) {
    if (!p) return;
    block_t *b = (block_t *)p - 1;
    b->next = free_list;
    free_list = b;
}

static unsigned rand_state = 12345;
void srand(unsigned seed) { rand_state = seed ? seed : 1; }
unsigned rand(void) {                         /* xorshift32 */
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;
    return rand_state;
}
