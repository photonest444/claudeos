/* Framebuffer graphics.
 * Everything is drawn into a 32-bit "target": normally the screen back
 * buffer, or an off-screen surface (each GUI window has one). Surfaces carry
 * an alpha channel, so they can be composited with transparency, scaled for
 * animations and given anti-aliased rounded corners. */
#ifndef GFX_H
#define GFX_H
#include "types.h"

#define FONT_W 8
#define FONT_H 16
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

extern int gfx_width, gfx_height;
extern const uint32_t vga_palette[16];

typedef struct { int w, h; uint32_t *px; } surface_t;     /* ARGB pixels */
typedef struct { uint32_t *tgt; int tw, th, x0, y0, x1, y1; } gfx_state_t;

bool gfx_init(void);                   /* false if no linear framebuffer */
bool gfx_available(void);

/* Drawing target and clipping. */
void       gfx_set_target(surface_t *s);    /* NULL = screen back buffer */
void       gfx_save(gfx_state_t *st);
void       gfx_restore(const gfx_state_t *st);
surface_t *gfx_surface_new(int w, int h);
void       gfx_surface_free(surface_t *s);
void       gfx_set_clip(int x, int y, int w, int h);
void       gfx_reset_clip(void);

/* Primitives (alpha: 0 = invisible, 255 = opaque). */
void gfx_pixel(int x, int y, uint32_t c);
void gfx_fill(int x, int y, int w, int h, uint32_t c);
void gfx_blend(int x, int y, int w, int h, uint32_t c, int alpha);
void gfx_rect(int x, int y, int w, int h, uint32_t c);
void gfx_round_fill(int x, int y, int w, int h, int r, uint32_t c);
void gfx_round_blend(int x, int y, int w, int h, int r, uint32_t c, int alpha);
void gfx_round_gradient(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom);
void gfx_round_outline(int x, int y, int w, int h, int r, uint32_t c, int alpha);
void gfx_circle(int cx, int cy, int r, uint32_t c, int alpha);
void gfx_ring(int cx, int cy, int ro, int ri, uint32_t c, int alpha);
void gfx_shadow(int x, int y, int w, int h, int r, int size, int alpha);
void gfx_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
uint32_t gfx_mix(uint32_t a, uint32_t b, int t);          /* t: 0 = a .. 255 = b */

/* Text (8x16 font). */
void gfx_char(int x, int y, char ch, uint32_t fg, uint32_t bg, bool transparent);
void gfx_text(int x, int y, const char *s, uint32_t fg);
void gfx_text_bold(int x, int y, const char *s, uint32_t fg);
void gfx_text_scaled(int x, int y, const char *s, uint32_t fg, int scale);

/* Surfaces -> current target. */
void gfx_blit(surface_t *s, int x, int y, int alpha);
void gfx_blit_scaled(surface_t *s, int x, int y, int w, int h, int alpha);
void gfx_blit_opaque(surface_t *s, int x, int y);
void gfx_surface_round(surface_t *s, int r);   /* anti-aliased transparent corners */

/* Screen only. */
void gfx_scroll_region(int y, int h, int pixels, uint32_t fill);  /* move rows up */
void gfx_flush(int x, int y, int w, int h);  /* copy back buffer -> video memory */
void gfx_flush_all(void);
#endif
