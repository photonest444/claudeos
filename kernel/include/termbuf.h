/* A character-cell terminal model with a small ANSI escape parser.
 * Used by the full-screen CLI console and by GUI terminal windows. */
#ifndef TERMBUF_H
#define TERMBUF_H
#include "types.h"

#define TB_ATTR(fg, bg) ((uint8_t)((fg) | ((bg) << 4)))
#define TB_DEFAULT_ATTR TB_ATTR(7, 0)

typedef struct termbuf {
    int cols, rows;
    int cx, cy;                 /* cursor position */
    uint8_t attr;               /* current colour attribute */
    uint16_t *cells;            /* char | attr << 8 */
    int esc_state, esc_n, esc_val[4];
    /* Hooks so the owner can render changes (any may be NULL). */
    void (*on_cell)(struct termbuf *t, int x, int y);
    void (*on_scroll)(struct termbuf *t);
    void (*on_redraw)(struct termbuf *t);
    void (*on_cursor)(struct termbuf *t, int oldx, int oldy);
    void *ctx;
} termbuf_t;

void termbuf_init(termbuf_t *t, int cols, int rows, uint16_t *cells);
void termbuf_putc(termbuf_t *t, char c);
void termbuf_write(termbuf_t *t, const char *s, size_t n);
void termbuf_clear(termbuf_t *t);
#endif
