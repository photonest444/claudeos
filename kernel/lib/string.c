/* Freestanding implementations of the usual libc string functions. */
#include "../include/string.h"

void *memcpy(void *d, const void *s, size_t n) {
    void *ret = d;
    /* rep movsd for the bulk, then the tail bytes */
    size_t dw = n / 4, rest = n % 4;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(dw) :: "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(rest) :: "memory");
    return ret;
}

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dp = d; const uint8_t *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];        /* overlapping, copy backwards */
    return d;
}

void *memset(void *d, int c, size_t n) {
    void *ret = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return ret;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (uint8_t)*a - (uint8_t)*b;
        if (!*a) return 0;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}

char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? NULL : (char *)s;
}

char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (; *s; s++) if (*s == (char)c) r = s;
    return (char *)(c ? r : s);
}

char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, nl)) return (char *)h;
    return nl ? NULL : (char *)h;
}

size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) { size_t c = len < n - 1 ? len : n - 1; memcpy(d, s, c); d[c] = 0; }
    return len;
}

size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strlen(d);
    if (dl >= n) return dl + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}

int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

int atoi(const char *s) {
    int sign = 1, v = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (isdigit(*s)) v = v * 10 + (*s++ - '0');
    return v * sign;
}
