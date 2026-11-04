/* TTY: binds a termbuf (output) to a keyboard input queue and a foreground task. */
#ifndef TTY_H
#define TTY_H
#include "types.h"
#include "termbuf.h"

#define TTY_INBUF 256

typedef struct tty {
    termbuf_t *tb;
    int inbuf[TTY_INBUF];
    volatile uint32_t in_head, in_tail;
    volatile int fg_pid;        /* user program currently in the foreground */
    volatile int hangup;        /* set when a GUI terminal window is closed */
    int is_console;
    int refs;                   /* GUI terminals: window + shell */
} tty_t;

extern tty_t *console_tty;

void tty_init(tty_t *t, termbuf_t *tb);
void tty_input(tty_t *t, int key);          /* called from IRQ/GUI context */
int  tty_getkey(tty_t *t);                  /* blocking, -1 on hangup */
int  tty_trygetkey(tty_t *t);               /* non-blocking, -1 if empty */
void tty_write(tty_t *t, const char *s, size_t n);
void tty_puts(tty_t *t, const char *s);
void tty_printf(tty_t *t, const char *fmt, ...);
int  tty_read_line(tty_t *t, char *buf, int max);   /* cooked input with echo */

/* Full-screen console (VGA text mode or framebuffer). */
void console_init(void);
void console_redraw(void);
void console_write(const char *s, size_t n);

/* Display mode: CLI console or GUI desktop. */
enum { DISPLAY_CLI = 0, DISPLAY_GUI = 1 };
extern volatile int display_mode;
extern volatile int display_switch_pending;
int  display_set_mode(int mode);           /* -1 if GUI unavailable */
void display_toggle(void);
#endif
