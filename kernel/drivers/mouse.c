/* PS/2 mouse: standard 3-byte packets on IRQ12. The absolute pointer position
 * is tracked here and every packet is queued for the GUI task. */
#include "../include/drivers.h"
#include "../include/io.h"
#include "../include/arch.h"

#define QSIZE 128
static mouse_event_t queue[QSIZE];
static volatile uint32_t q_head, q_tail;
static uint8_t packet[3];
static int cycle;
static volatile int mx, my, mbuttons;
static int max_x = 1023, max_y = 767;
static int speed = 100;                     /* percent, set from GUI settings */

static void wait_write(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ; }
static void wait_read(void)  { for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++) ; }

static void mouse_cmd(uint8_t c) {
    wait_write(); outb(0x64, 0xD4);       /* next byte goes to the mouse */
    wait_write(); outb(0x60, c);
    wait_read();  inb(0x60);              /* ACK (0xFA) */
}

static void mouse_irq(regs_t *r) {
    UNUSED(r);
    uint8_t status = inb(0x64);
    if (!(status & 1) || !(status & 0x20)) return;
    uint8_t b = inb(0x60);
    if (cycle == 0 && !(b & 0x08)) return;   /* resync: bit 3 is always set in byte 0 */
    packet[cycle++] = b;
    if (cycle < 3) return;
    cycle = 0;
    if (packet[0] & 0xC0) return;            /* overflow, drop */
    int dx = packet[1] - ((packet[0] & 0x10) ? 256 : 0);
    int dy = packet[2] - ((packet[0] & 0x20) ? 256 : 0);
    dx = dx * speed / 100; dy = dy * speed / 100;
    mx += dx; my -= dy;                      /* PS/2 Y axis points up */
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > max_x) mx = max_x;
    if (my > max_y) my = max_y;
    mbuttons = packet[0] & 7;
    uint32_t next = (q_head + 1) % QSIZE;
    if (next != q_tail) {
        queue[q_head] = (mouse_event_t){ mx, my, mbuttons };
        q_head = next;
    }
}

void mouse_init(void) {
    wait_write(); outb(0x64, 0xA8);          /* enable auxiliary device */
    wait_write(); outb(0x64, 0x20);          /* read controller config */
    wait_read();
    uint8_t cfg = inb(0x60) | 0x02;          /* enable IRQ12 */
    cfg &= ~0x20;                            /* enable mouse clock */
    wait_write(); outb(0x64, 0x60);
    wait_write(); outb(0x60, cfg);
    mouse_cmd(0xF6);                         /* defaults */
    mouse_cmd(0xF4);                         /* enable data reporting */
    irq_install(12, mouse_irq);
}

void mouse_set_bounds(int w, int h) {
    max_x = w - 1; max_y = h - 1;
    mx = w / 2; my = h / 2;
}

int mouse_poll(mouse_event_t *ev) {
    uint32_t f = irq_save();
    int got = 0;
    if (q_tail != q_head) { *ev = queue[q_tail]; q_tail = (q_tail + 1) % QSIZE; got = 1; }
    irq_restore(f);
    return got;
}

void mouse_get(int *x, int *y, int *buttons) { *x = mx; *y = my; *buttons = mbuttons; }

void mouse_set_speed(int percent) { speed = percent < 25 ? 25 : percent > 400 ? 400 : percent; }
