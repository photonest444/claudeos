/* Kernel logging: goes to the serial port and the text console. */
#ifndef KPRINTF_H
#define KPRINTF_H
#include "types.h"
#include "../../common/format.h"

void kprintf(const char *fmt, ...);
void kputs(const char *s);
void panic(const char *fmt, ...) __attribute__((noreturn));
#endif
