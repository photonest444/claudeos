/* Minimal string/memory routines for the kernel. */
#ifndef KSTRING_H
#define KSTRING_H
#include "types.h"

void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
size_t strlcpy(char *d, const char *s, size_t n);   /* always NUL-terminates */
size_t strlcat(char *d, const char *s, size_t n);
int    toupper(int c);
int    tolower(int c);
int    isdigit(int c);
int    isspace(int c);
int    atoi(const char *s);
char  *strstr(const char *h, const char *n);
#endif
