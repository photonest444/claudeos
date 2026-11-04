/* Full-screen CLI console. Renders the console termbuf either into the VGA
 * text buffer (0xB8000, fallback) or onto the framebuffer with the 8x16 font.
 * Also owns the CLI/GUI display mode switch. */
#include "../include/tty.h"
#include "../include/gfx.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/drivers.h"
#include "../include/kprintf.h"

#define MAX_COLS 200
#define MAX_ROWS 100

static termbuf_t con_tb;
static uint16_t  con_cells[MAX_COLS * MAX_ROWS];
static tty_t     con_tty;
tty_t *console_tty;
static bool use_fb, ready;
static volatile uint16_t *const vga = (uint16_t *)0xB8000;
static int cur_x = -1, cur_y = -1;        /* where the cursor is currently drawn */

volatile int display_mode = DISPLAY_CLI;
volatile int display_switch_pending;

static void draw_cell(termbuf_t *t, int x, int y, bool cursor) {
    uint16_t v = t->cells[y * t->cols + x];
    if (!use_fb) { vga[y * t->cols + x] = v; return; }
    uint8_t attr = v >> 8;
    uint32_t fg = vga_palette[attr & 15], bg = vga_palette[attr >> 4];
    gfx_state_t st;                     /* the GUI task may be drawing elsewhere */
    gfx_save(&st);
    gfx_set_target(NULL);
    gfx_char(x * FONT_W, y * FONT_H, (char)(v & 0xFF), fg, bg, false);
    if (cursor) gfx_fill(x * FONT_W, y * FONT_H + FONT_H - 3, FONT_W, 2, fg);
    gfx_restore(&st);
    gfx_flush(x * FONT_W, y * FONT_H, FONT_W, FONT_H);
}

static void hook_cell(termbuf_t *t, int x, int y) {
    if (display_mode == DISPLAY_CLI) draw_cell(t, x, y, false);
}

static void hook_cursor(termbuf_t *t, int ox, int oy) {
    if (display_mode != DISPLAY_CLI) return;
    if (!use_fb) {                              /* move the hardware cursor */
        uint16_t pos = t->cy * t->cols + t->cx;
        outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
        outb(0x3D4, 0x0E); outb(0x3D5, pos >> 8);
        return;
    }
    UNUSED(ox); UNUSED(oy);
    if (cur_x >= 0) draw_cell(t, cur_x, cur_y, false);   /* erase the old cursor */
    draw_cell(t, t->cx, t->cy, true);
    cur_x = t->cx;
    cur_y = t->cy;
}

static void hook_redraw(termbuf_t *t) {
    if (display_mode != DISPLAY_CLI) return;
    if (use_fb) {
        gfx_state_t st;
        gfx_save(&st);
        gfx_set_target(NULL);
        gfx_fill(0, 0, gfx_width, gfx_height, vga_palette[0]);
        for (int y = 0; y < t->rows; y++)
            for (int x = 0; x < t->cols; x++) {
                uint16_t v = t->cells[y * t->cols + x];
                gfx_char(x * FONT_W, y * FONT_H, (char)(v & 0xFF),
                         vga_palette[(v >> 8) & 15], vga_palette[v >> 12], false);
            }
        gfx_fill(t->cx * FONT_W, t->cy * FONT_H + FONT_H - 3, FONT_W, 2, vga_palette[t->attr & 15]);
        cur_x = t->cx;
        cur_y = t->cy;
        gfx_restore(&st);
        gfx_flush_all();
    } else {
        for (int i = 0; i < t->cols * t->rows; i++) vga[i] = t->cells[i];
    }
}

static void hook_scroll(termbuf_t *t) {
    if (display_mode != DISPLAY_CLI) return;
    if (!use_fb) { hook_redraw(t); return; }
    gfx_state_t st;
    gfx_save(&st);
    gfx_set_target(NULL);
    gfx_scroll_region(0, t->rows * FONT_H, FONT_H, vga_palette[t->attr >> 4]);
    for (int x = 0; x < t->cols; x++) {
        uint16_t v = t->cells[(t->rows - 1) * t->cols + x];
        gfx_char(x * FONT_W, (t->rows - 1) * FONT_H, (char)(v & 0xFF),
                 vga_palette[(v >> 8) & 15], vga_palette[v >> 12], false);
    }
    gfx_restore(&st);
    gfx_flush(0, 0, gfx_width, t->rows * FONT_H);
    /* The drawn cursor moved up one row together with the pixels: erase it. */
    if (cur_x >= 0 && cur_y > 0) draw_cell(t, cur_x, cur_y - 1, false);
    cur_x = -1;
}

void console_init(void) {
    int cols = 80, rows = 25;
    use_fb = gfx_available();
    if (use_fb) { cols = MIN(gfx_width / FONT_W, MAX_COLS); rows = MIN(gfx_height / FONT_H, MAX_ROWS); }
    con_tb.on_cell = hook_cell;
    con_tb.on_cursor = hook_cursor;
    con_tb.on_redraw = hook_redraw;
    con_tb.on_scroll = hook_scroll;
    termbuf_init(&con_tb, cols, rows, con_cells);
    tty_init(&con_tty, &con_tb);
    con_tty.is_console = 1;
    console_tty = &con_tty;
    ready = true;
}

void console_write(const char *s, size_t n) {
    uint32_t f = irq_save();
    serial_write(s, n);
    if (ready) termbuf_write(&con_tb, s, n);
    irq_restore(f);
}

void console_redraw(void) {
    uint32_t f = irq_save();
    hook_redraw(&con_tb);
    hook_cursor(&con_tb, con_tb.cx, con_tb.cy);
    irq_restore(f);
}

int display_set_mode(int mode) {
    if (mode == DISPLAY_GUI && !gfx_available()) return -1;
    if (mode != display_mode) {
        display_mode = mode;
        display_switch_pending = 1;     /* the display task repaints the screen */
    }
    return 0;
}

void display_toggle(void) {
    if (display_set_mode(display_mode == DISPLAY_CLI ? DISPLAY_GUI : DISPLAY_CLI) < 0)
        console_write("\n[GUI not available: no framebuffer, booted in text mode]\n", 59);
}
