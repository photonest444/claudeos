/* Character-cell terminal model. Understands \n \r \b \t and a subset of ANSI
 * escapes: ESC[...m (colours), ESC[2J (clear), ESC[H (home), ESC[K (erase EOL). */
#include "../include/termbuf.h"
#include "../include/string.h"

/* ANSI colour order -> VGA palette index */
static const uint8_t ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

void termbuf_init(termbuf_t *t, int cols, int rows, uint16_t *cells) {
    t->cols = cols; t->rows = rows; t->cells = cells;
    t->attr = TB_DEFAULT_ATTR;
    t->esc_state = 0;
    termbuf_clear(t);
}

void termbuf_clear(termbuf_t *t) {
    for (int i = 0; i < t->cols * t->rows; i++) t->cells[i] = ' ' | (t->attr << 8);
    t->cx = t->cy = 0;
    if (t->on_redraw) t->on_redraw(t);
}

static void scroll(termbuf_t *t) {
    memmove(t->cells, t->cells + t->cols, (t->rows - 1) * t->cols * 2);
    for (int x = 0; x < t->cols; x++) t->cells[(t->rows - 1) * t->cols + x] = ' ' | (t->attr << 8);
    t->cy = t->rows - 1;
    if (t->on_scroll) t->on_scroll(t);
    else if (t->on_redraw) t->on_redraw(t);
}

static void sgr(termbuf_t *t, int v) {
    uint8_t fg = t->attr & 15, bg = t->attr >> 4;
    if (v == 0) { fg = 7; bg = 0; }
    else if (v == 1) fg |= 8;
    else if (v >= 30 && v <= 37) fg = ansi2vga[v - 30] | (fg & 8);
    else if (v >= 90 && v <= 97) fg = ansi2vga[v - 90] | 8;
    else if (v >= 40 && v <= 47) bg = ansi2vga[v - 40];
    else if (v == 39) fg = 7;
    else if (v == 49) bg = 0;
    t->attr = TB_ATTR(fg, bg);
}

static void escape(termbuf_t *t, char cmd) {
    int n = t->esc_n + 1;
    switch (cmd) {
    case 'm': for (int i = 0; i < n; i++) sgr(t, t->esc_val[i]); break;
    case 'J': if (t->esc_val[0] == 2) { uint8_t a = t->attr; t->attr = TB_ATTR(7, a >> 4); termbuf_clear(t); t->attr = a; } break;
    case 'H': t->cx = 0; t->cy = 0; break;
    case 'K':
        for (int x = t->cx; x < t->cols; x++) {
            t->cells[t->cy * t->cols + x] = ' ' | (t->attr << 8);
            if (t->on_cell) t->on_cell(t, x, t->cy);
        }
        break;
    }
}

static void putc_raw(termbuf_t *t, char c) {
    if (t->esc_state == 1) {
        if (c == '[') { t->esc_state = 2; t->esc_n = 0; memset(t->esc_val, 0, sizeof(t->esc_val)); }
        else t->esc_state = 0;
        return;
    }
    if (t->esc_state == 2) {
        if (c >= '0' && c <= '9') t->esc_val[t->esc_n] = t->esc_val[t->esc_n] * 10 + (c - '0');
        else if (c == ';') { if (t->esc_n < 3) t->esc_n++; }
        else { escape(t, c); t->esc_state = 0; }
        return;
    }
    switch (c) {
    case 0x1B: t->esc_state = 1; return;
    case '\n': t->cx = 0; t->cy++; break;
    case '\r': t->cx = 0; break;
    case '\b': if (t->cx > 0) t->cx--; break;
    case '\t': t->cx = (t->cx + 8) & ~7; if (t->cx >= t->cols) { t->cx = 0; t->cy++; } break;
    default:
        if ((uint8_t)c < 32) return;
        t->cells[t->cy * t->cols + t->cx] = (uint8_t)c | (t->attr << 8);
        if (t->on_cell) t->on_cell(t, t->cx, t->cy);
        if (++t->cx >= t->cols) { t->cx = 0; t->cy++; }
    }
    if (t->cy >= t->rows) scroll(t);
}

void termbuf_write(termbuf_t *t, const char *s, size_t n) {
    int ox = t->cx, oy = t->cy;
    while (n--) putc_raw(t, *s++);
    if (t->on_cursor) t->on_cursor(t, ox, oy);
}

void termbuf_putc(termbuf_t *t, char c) { termbuf_write(t, &c, 1); }
