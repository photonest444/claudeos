/* Kernel log. Before the console exists, output goes to the serial port only. */
#include "../include/kprintf.h"
#include "../include/tty.h"
#include "../include/drivers.h"
#include "../include/io.h"
#include "../include/string.h"

void kputs(const char *s) { console_write(s, strlen(s)); }

void kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kputs(buf);
}

void panic(const char *fmt, ...) {
    __asm__ volatile("cli");
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    display_mode = DISPLAY_CLI;        /* make sure the message is visible */
    console_redraw();
    kputs("\n\x1b[97;41m *** KERNEL PANIC *** \x1b[0m\n");
    kputs(buf);
    kputs("\nSystem halted.\n");
    for (;;) cpu_halt();
}
