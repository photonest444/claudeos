/* TTY layer: keyboard input queue + output to a termbuf.
 * Ctrl+C kills the TTY's foreground user program. */
#include "../include/tty.h"
#include "../include/task.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/drivers.h"
#include "../include/kprintf.h"

void tty_init(tty_t *t, termbuf_t *tb) {
    memset(t, 0, sizeof(*t));
    t->tb = tb;
}

void tty_input(tty_t *t, int key) {
    if (key == 3 && t->fg_pid) {               /* Ctrl+C */
        int pid = t->fg_pid;
        t->fg_pid = 0;
        tty_puts(t, "^C\n");
        task_kill(pid);
        return;
    }
    uint32_t f = irq_save();
    uint32_t next = (t->in_head + 1) % TTY_INBUF;
    if (next != t->in_tail) { t->inbuf[t->in_head] = key; t->in_head = next; }
    irq_restore(f);
}

int tty_trygetkey(tty_t *t) {
    uint32_t f = irq_save();
    int k = -1;
    if (t->in_tail != t->in_head) { k = t->inbuf[t->in_tail]; t->in_tail = (t->in_tail + 1) % TTY_INBUF; }
    irq_restore(f);
    return k;
}

int tty_getkey(tty_t *t) {
    for (;;) {
        int k = tty_trygetkey(t);
        if (k >= 0) return k;
        if (t->hangup) return -1;
        task_sleep(10);
    }
}

void tty_write(tty_t *t, const char *s, size_t n) {
    if (t->is_console) { console_write(s, n); return; }
    uint32_t f = irq_save();
    termbuf_write(t->tb, s, n);
    irq_restore(f);
}

void tty_puts(tty_t *t, const char *s) { tty_write(t, s, strlen(s)); }

void tty_printf(tty_t *t, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tty_write(t, buf, MIN(n, (int)sizeof(buf) - 1));
}

/* Canonical line input for user programs: echo, backspace, Enter ends. */
int tty_read_line(tty_t *t, char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        int k = tty_getkey(t);
        if (k < 0) break;
        if (k == '\n') { tty_puts(t, "\n"); buf[n++] = '\n'; break; }
        if (k == '\b') { if (n) { n--; tty_puts(t, "\b \b"); } continue; }
        if (k == 4 && n == 0) break;           /* Ctrl+D = end of input */
        if (k >= 32 && k < 127) { buf[n++] = (char)k; char c = (char)k; tty_write(t, &c, 1); }
    }
    return n;
}
