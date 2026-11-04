/* COM1 serial port (115200 8N1). Mirrors the console output so the OS can be
 * driven headless (qemu -serial stdio) and feeds received bytes into the
 * CLI console as keyboard input. */
#include "../include/drivers.h"
#include "../include/io.h"
#include "../include/arch.h"
#include "../include/tty.h"

#define COM1 0x3F8
static bool present;

static void serial_irq(regs_t *r) {
    UNUSED(r);
    while (inb(COM1 + 5) & 1) {
        int c = inb(COM1);
        if (c == '\r') c = '\n';
        if (c == 0x7F) c = '\b';
        if (console_tty) tty_input(console_tty, c);
    }
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x0B);   /* RTS/DSR, OUT2 (needed for IRQs) */
    present = inb(COM1 + 5) != 0xFF;
}

void serial_enable_input(void) {
    if (!present) return;
    outb(COM1 + 1, 0x01);   /* IRQ on received data */
    irq_install(4, serial_irq);
}

void serial_putc(char c) {
    if (!present) return;
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) ;
    outb(COM1, c);
}

void serial_write(const char *s, size_t n) { while (n--) serial_putc(*s++); }
