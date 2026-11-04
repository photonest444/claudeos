/* Linear framebuffer graphics.
 *
 * All drawing goes to a 32-bit ARGB target: the screen back buffer (copied to
 * video memory by gfx_flush*) or an off-screen surface. Rounded shapes are
 * anti-aliased by 4x4 supersampling of the corner pixels only, which keeps
 * them cheap enough to redraw every frame. */
#include "../include/gfx.h"
#include "../include/mm.h"
#include "../include/io.h"
#include "../include/multiboot.h"
#include "../include/string.h"
#include "../include/kprintf.h"

extern const uint8_t font8x16[128][16];

#define OPAQUE 0xFF000000u

int gfx_width, gfx_height;
static uint8_t  *fb;
static uint32_t  fb_pitch, fb_bpp;
static uint32_t *back;                       /* screen back buffer */
static uint32_t *tgt;                        /* current target */
static int tw, th;
static int clip_x0, clip_y0, clip_x1, clip_y1;   /* clip rectangle [x0,x1) x [y0,y1) */
static bool ok;

/* Terminal palette (VGA order), modern colours. */
const uint32_t vga_palette[16] = {
    0x16171A, 0x4C8DF6, 0x3FB950, 0x2BB3C0, 0xF85149, 0xBC8CFF, 0xD29922, 0xC9D1D9,
    0x6E7681, 0x79C0FF, 0x56D364, 0x56D4DD, 0xFF7B72, 0xD2A8FF, 0xE3B341, 0xF0F6FC,
};

bool gfx_init(void) {
    if (boot_info.fb_type != 1 || !boot_info.fb_addr || boot_info.fb_addr >> 32) return false;
    if (boot_info.fb_bpp != 32 && boot_info.fb_bpp != 24) return false;
    fb = (uint8_t *)(uint32_t)boot_info.fb_addr;
    fb_pitch = boot_info.fb_pitch;
    fb_bpp = boot_info.fb_bpp;
    gfx_width = boot_info.fb_width;
    gfx_height = boot_info.fb_height;
    paging_map_identity_range((uint32_t)fb, fb_pitch * gfx_height);
    uint32_t pages = ALIGN_UP((uint32_t)gfx_width * gfx_height * 4, PAGE_SIZE) / PAGE_SIZE;
    back = (uint32_t *)pmm_alloc_contiguous(pages);
    if (!back) return false;
    memset(back, 0, pages * PAGE_SIZE);
    gfx_set_target(NULL);
    ok = true;
    kprintf("[gfx] framebuffer %dx%dx%d at %p\n", gfx_width, gfx_height, fb_bpp, (uint32_t)fb);
    return true;
}

bool gfx_available(void) { return ok; }

/* ---- target, state, surfaces ------------------------------------------- */
void gfx_set_clip(int x, int y, int w, int h) {
    clip_x0 = MAX(x, 0); clip_y0 = MAX(y, 0);
    clip_x1 = MIN(x + w, tw); clip_y1 = MIN(y + h, th);
}

void gfx_reset_clip(void) { gfx_set_clip(0, 0, tw, th); }

void gfx_set_target(surface_t *s) {
    uint32_t f = irq_save();                    /* tgt/tw/th must change together */
    if (s) { tgt = s->px; tw = s->w; th = s->h; }
    else   { tgt = back; tw = gfx_width; th = gfx_height; }
    gfx_reset_clip();
    irq_restore(f);
}

void gfx_save(gfx_state_t *st) {
    *st = (gfx_state_t){ tgt, tw, th, clip_x0, clip_y0, clip_x1, clip_y1 };
}

void gfx_restore(const gfx_state_t *st) {
    tgt = st->tgt; tw = st->tw; th = st->th;
    clip_x0 = st->x0; clip_y0 = st->y0; clip_x1 = st->x1; clip_y1 = st->y1;
}

surface_t *gfx_surface_new(int w, int h) {
    surface_t *s = kmalloc(sizeof(surface_t));
    if (!s) return NULL;
    s->px = kmalloc((size_t)w * h * 4);
    if (!s->px) { kfree(s); return NULL; }
    s->w = w; s->h = h;
    memset(s->px, 0, (size_t)w * h * 4);
    return s;
}

void gfx_surface_free(surface_t *s) {
    if (!s) return;
    kfree(s->px);
    kfree(s);
}

/* ---- pixels and blending ---------------------------------------------- */
static inline uint32_t mix(uint32_t d, uint32_t s, uint32_t a) {
    if (a >= 255) return s | OPAQUE;
    if (a == 0) return d;
    uint32_t rb = (((s & 0xFF00FF) * a + (d & 0xFF00FF) * (255 - a)) >> 8) & 0xFF00FF;
    uint32_t g  = (((s & 0x00FF00) * a + (d & 0x00FF00) * (255 - a)) >> 8) & 0x00FF00;
    uint32_t da = d >> 24;
    uint32_t oa = da == 255 ? 255 : a + da * (255 - a) / 255;   /* alpha "over" */
    return rb | g | (oa << 24);
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a & 0xFFFFFF;
    if (t >= 255) return b & 0xFFFFFF;
    return mix(a | OPAQUE, b, (uint32_t)t) & 0xFFFFFF;
}

void gfx_pixel(int x, int y, uint32_t c) {
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1)
        tgt[y * tw + x] = c | OPAQUE;
}

static inline void blend_pixel(int x, int y, uint32_t c, uint32_t a) {
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) {
        uint32_t *p = &tgt[y * tw + x];
        *p = mix(*p, c, a);
    }
}

void gfx_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = MAX(x, clip_x0), y0 = MAX(y, clip_y0);
    int x1 = MIN(x + w, clip_x1), y1 = MIN(y + h, clip_y1);
    c |= OPAQUE;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = tgt + yy * tw;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void gfx_blend(int x, int y, int w, int h, uint32_t c, int alpha) {
    if (alpha >= 255) { gfx_fill(x, y, w, h, c); return; }
    if (alpha <= 0) return;
    int x0 = MAX(x, clip_x0), y0 = MAX(y, clip_y0);
    int x1 = MIN(x + w, clip_x1), y1 = MIN(y + h, clip_y1);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = tgt + yy * tw;
        for (int xx = x0; xx < x1; xx++) row[xx] = mix(row[xx], c, alpha);
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t c) {
    gfx_fill(x, y, w, 1, c);
    gfx_fill(x, y + h - 1, w, 1, c);
    gfx_fill(x, y, 1, h, c);
    gfx_fill(x + w - 1, y, 1, h, c);
}

/* Coverage (0..16) of pixel (i, j) inside the top-left corner square of a
 * rounded rectangle with radius r, by 4x4 supersampling. Other corners are
 * mirror images, so this one function serves all four. */
static int corner_cov(int i, int j, int r) {
    if (r <= 0) return 16;
    if (i < 0 || j < 0) return 0;
    int n = 0, R2 = (r * 8) * (r * 8);
    for (int a = 0; a < 4; a++) {
        int sx = i * 8 + 2 * a + 1 - r * 8;
        for (int b = 0; b < 4; b++) {
            int sy = j * 8 + 2 * b + 1 - r * 8;
            if (sx * sx + sy * sy <= R2) n++;
        }
    }
    return n;
}

static uint32_t row_color(uint32_t top, uint32_t bottom, int j, int h) {
    return h > 1 ? gfx_mix(top, bottom, j * 255 / (h - 1)) : top;
}

/* Rounded rectangle with a vertical gradient and global alpha. */
static void round_core(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    bool grad = top != bottom;
    for (int j = 0; j < h; j++) {
        uint32_t c = grad ? row_color(top, bottom, j, h) : top;
        int cj = j < r ? j : (j >= h - r ? h - 1 - j : -1);   /* row inside a corner band? */
        if (cj < 0) { gfx_blend(x, y + j, w, 1, c, alpha); continue; }
        gfx_blend(x + r, y + j, w - 2 * r, 1, c, alpha);
        for (int i = 0; i < r; i++) {
            int k = corner_cov(i, cj, r);
            if (!k) continue;
            int a = alpha * k / 16;
            blend_pixel(x + i, y + j, c, a);
            blend_pixel(x + w - 1 - i, y + j, c, a);
        }
    }
}

void gfx_round_fill(int x, int y, int w, int h, int r, uint32_t c) { round_core(x, y, w, h, r, c, c, 255); }
void gfx_round_blend(int x, int y, int w, int h, int r, uint32_t c, int alpha) { round_core(x, y, w, h, r, c, c, alpha); }
void gfx_round_gradient(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom) {
    round_core(x, y, w, h, r, top, bottom, 255);
}

void gfx_circle(int cx, int cy, int r, uint32_t c, int alpha) {
    round_core(cx - r, cy - r, 2 * r, 2 * r, r, c, c, alpha);
}

/* 1-pixel anti-aliased outline = outer shape minus the shape inset by 1. */
void gfx_round_outline(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    if (w <= 1 || h <= 1) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_blend(x + r, y, w - 2 * r, 1, c, alpha);
    gfx_blend(x + r, y + h - 1, w - 2 * r, 1, c, alpha);
    gfx_blend(x, y + r, 1, h - 2 * r, c, alpha);
    gfx_blend(x + w - 1, y + r, 1, h - 2 * r, c, alpha);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int inner = (i == 0 || j == 0) ? 0 : (r - 1 <= 0 ? 16 : corner_cov(i - 1, j - 1, r - 1));
            int k = corner_cov(i, j, r) - inner;
            if (k <= 0) continue;
            int a = alpha * k / 16;
            blend_pixel(x + i, y + j, c, a);
            blend_pixel(x + w - 1 - i, y + j, c, a);
            blend_pixel(x + i, y + h - 1 - j, c, a);
            blend_pixel(x + w - 1 - i, y + h - 1 - j, c, a);
        }
}

/* Anti-aliased ring (annulus) centred on the pixel corner (cx, cy). */
void gfx_ring(int cx, int cy, int ro, int ri, uint32_t c, int alpha) {
    int RO = (ro * 8) * (ro * 8), RI = (ri * 8) * (ri * 8);
    for (int y = -ro; y < ro; y++)
        for (int x = -ro; x < ro; x++) {
            int n = 0;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++) {
                    int sx = x * 8 + 2 * a + 1, sy = y * 8 + 2 * b + 1, d = sx * sx + sy * sy;
                    if (d <= RO && d > RI) n++;
                }
            if (n) blend_pixel(cx + x, cy + y, c, alpha * n / 16);
        }
}

/* Soft shadow: concentric 1-px rings with quadratically fading alpha. */
void gfx_shadow(int x, int y, int w, int h, int r, int size, int alpha) {
    for (int i = 1; i <= size; i++) {
        int a = alpha * (size - i + 1) * (size - i + 1) / (size * size);
        int rx = x - i, ry = y - i + 2, rw = w + 2 * i, rh = h + 2 * i, R = r + i;
        if (a <= 0) continue;
        gfx_blend(rx + R, ry, rw - 2 * R, 1, 0, a);
        gfx_blend(rx + R, ry + rh - 1, rw - 2 * R, 1, 0, a);
        gfx_blend(rx, ry + R, 1, rh - 2 * R, 0, a);
        gfx_blend(rx + rw - 1, ry + R, 1, rh - 2 * R, 0, a);
        int outer = 4 * R * R, inner = 4 * (R - 1) * (R - 1);
        for (int j = 0; j < R; j++)
            for (int k = 0; k < R; k++) {
                int dx = 2 * k + 1 - 2 * R, dy = 2 * j + 1 - 2 * R, d = dx * dx + dy * dy;
                if (d > outer || d <= inner) continue;
                blend_pixel(rx + k, ry + j, 0, a);
                blend_pixel(rx + rw - 1 - k, ry + j, 0, a);
                blend_pixel(rx + k, ry + rh - 1 - j, 0, a);
                blend_pixel(rx + rw - 1 - k, ry + rh - 1 - j, 0, a);
            }
    }
}

void gfx_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int i = 0; i < h; i++) gfx_fill(x, y + i, w, 1, row_color(top, bottom, i, h));
}

/* ---- text ------------------------------------------------------------- */
void gfx_char(int x, int y, char ch, uint32_t fg, uint32_t bg, bool transparent) {
    uint8_t c = (uint8_t)ch;
    const uint8_t *g = font8x16[c < 128 ? c : '?'];
    fg |= OPAQUE; bg |= OPAQUE;
    if (x >= clip_x0 && y >= clip_y0 && x + FONT_W <= clip_x1 && y + FONT_H <= clip_y1) {
        for (int r = 0; r < FONT_H; r++) {      /* fast path: fully visible */
            uint32_t *row = tgt + (y + r) * tw + x;
            uint8_t bits = g[r];
            for (int b = 0; b < FONT_W; b++) {
                if (bits & (0x80 >> b)) row[b] = fg;
                else if (!transparent) row[b] = bg;
            }
        }
        return;
    }
    for (int r = 0; r < FONT_H; r++)
        for (int b = 0; b < FONT_W; b++) {
            if (g[r] & (0x80 >> b)) gfx_pixel(x + b, y + r, fg);
            else if (!transparent) gfx_pixel(x + b, y + r, bg);
        }
}

void gfx_text(int x, int y, const char *s, uint32_t fg) {
    for (; *s; s++, x += FONT_W) gfx_char(x, y, *s, fg, 0, true);
}

void gfx_text_bold(int x, int y, const char *s, uint32_t fg) {
    gfx_text(x, y, s, fg);
    gfx_text(x + 1, y, s, fg);
}

void gfx_text_scaled(int x, int y, const char *s, uint32_t fg, int scale) {
    for (; *s; s++, x += FONT_W * scale) {
        uint8_t c = (uint8_t)*s;
        const uint8_t *g = font8x16[c < 128 ? c : '?'];
        for (int r = 0; r < FONT_H; r++)
            for (int b = 0; b < FONT_W; b++)
                if (g[r] & (0x80 >> b)) gfx_fill(x + b * scale, y + r * scale, scale, scale, fg);
    }
}

/* ---- surfaces ---------------------------------------------------------- */
void gfx_surface_round(surface_t *s, int r) {
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            uint32_t k = (uint32_t)corner_cov(i, j, r) * 255 / 16;
            if (k >= 255) continue;
            int xs[2] = { i, s->w - 1 - i }, ys[2] = { j, s->h - 1 - j };
            for (int a = 0; a < 2; a++)
                for (int b = 0; b < 2; b++) {
                    uint32_t *p = &s->px[ys[b] * s->w + xs[a]];
                    if ((*p >> 24) > k) *p = (*p & 0xFFFFFF) | (k << 24);
                }
        }
}

static inline void put_src(uint32_t *d, uint32_t p, int alpha) {
    uint32_t a = p >> 24;
    if (alpha < 255) a = a * (uint32_t)alpha / 255;
    if (a >= 255) *d = p;
    else if (a) *d = mix(*d, p, a);
}

void gfx_blit(surface_t *s, int x, int y, int alpha) {
    int x0 = MAX(x, clip_x0), y0 = MAX(y, clip_y0);
    int x1 = MIN(x + s->w, clip_x1), y1 = MIN(y + s->h, clip_y1);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *src = s->px + (yy - y) * s->w - x, *dst = tgt + yy * tw;
        for (int xx = x0; xx < x1; xx++) put_src(&dst[xx], src[xx], alpha);
    }
}

void gfx_blit_scaled(surface_t *s, int x, int y, int w, int h, int alpha) {
    static int xmap[4096];
    if (w <= 0 || h <= 0 || w > 4096) return;
    for (int i = 0; i < w; i++) xmap[i] = i * s->w / w;
    int x0 = MAX(x, clip_x0), y0 = MAX(y, clip_y0);
    int x1 = MIN(x + w, clip_x1), y1 = MIN(y + h, clip_y1);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *src = s->px + ((yy - y) * s->h / h) * s->w, *dst = tgt + yy * tw;
        for (int xx = x0; xx < x1; xx++) put_src(&dst[xx], src[xmap[xx - x]], alpha);
    }
}

void gfx_blit_opaque(surface_t *s, int x, int y) {
    int x0 = MAX(x, clip_x0), y0 = MAX(y, clip_y0);
    int x1 = MIN(x + s->w, clip_x1), y1 = MIN(y + s->h, clip_y1);
    if (x0 >= x1) return;
    for (int yy = y0; yy < y1; yy++)
        memcpy(tgt + yy * tw + x0, s->px + (yy - y) * s->w + (x0 - x), (x1 - x0) * 4);
}

/* ---- screen ------------------------------------------------------------- */
void gfx_scroll_region(int y, int h, int pixels, uint32_t fill) {
    fill |= OPAQUE;
    if (pixels >= h) {
        for (int i = 0; i < h * gfx_width; i++) back[y * gfx_width + i] = fill;
        return;
    }
    memmove(back + y * gfx_width, back + (y + pixels) * gfx_width,
            (uint32_t)(h - pixels) * gfx_width * 4);
    uint32_t *p = back + (y + h - pixels) * gfx_width;
    for (int i = 0; i < pixels * gfx_width; i++) p[i] = fill;
}

void gfx_flush(int x, int y, int w, int h) {
    if (!ok) return;
    int x0 = MAX(x, 0), y0 = MAX(y, 0);
    int x1 = MIN(x + w, gfx_width), y1 = MIN(y + h, gfx_height);
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *src = back + yy * gfx_width + x0;
        if (fb_bpp == 32) {
            memcpy(fb + yy * fb_pitch + x0 * 4, src, (x1 - x0) * 4);
        } else {
            uint8_t *dst = fb + yy * fb_pitch + x0 * 3;
            for (int xx = x0; xx < x1; xx++, src++) {
                *dst++ = *src & 0xFF; *dst++ = (*src >> 8) & 0xFF; *dst++ = (*src >> 16) & 0xFF;
            }
        }
    }
}

void gfx_flush_all(void) { gfx_flush(0, 0, gfx_width, gfx_height); }
