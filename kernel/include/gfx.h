/* Framebuffer graphics: a 32-bit back buffer + primitives + 8x16 font. */
#ifndef GFX_H
#define GFX_H
#include "types.h"

#define FONT_W 8
#define FONT_H 16
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

extern int gfx_width, gfx_height;
extern const uint32_t vga_palette[16];

bool gfx_init(void);                   /* false if no linear framebuffer */
bool gfx_available(void);
void gfx_set_clip(int x, int y, int w, int h);
void gfx_reset_clip(void);
void gfx_pixel(int x, int y, uint32_t c);
void gfx_fill(int x, int y, int w, int h, uint32_t c);
void gfx_rect(int x, int y, int w, int h, uint32_t c);
void gfx_bevel(int x, int y, int w, int h, bool raised);   /* 3D border */
void gfx_char(int x, int y, char ch, uint32_t fg, uint32_t bg, bool transparent);
void gfx_text(int x, int y, const char *s, uint32_t fg);
void gfx_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_scroll_region(int y, int h, int pixels, uint32_t fill);  /* move rows up */
void gfx_flush(int x, int y, int w, int h);  /* copy back buffer -> screen */
void gfx_flush_all(void);
#endif
