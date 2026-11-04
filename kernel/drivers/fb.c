/* Linear framebuffer graphics. Everything is drawn into a 32-bit back buffer
 * in RAM and copied to video memory with gfx_flush*(), which avoids flicker
 * and converts to the real pixel format (32 or 24 bpp). */
#include "../include/gfx.h"
#include "../include/mm.h"
#include "../include/multiboot.h"
#include "../include/string.h"
#include "../include/kprintf.h"

extern const uint8_t font8x16[128][16];

int gfx_width, gfx_height;
static uint8_t  *fb;
static uint32_t  fb_pitch, fb_bpp;
static uint32_t *back;
static int clip_x0, clip_y0, clip_x1, clip_y1;   /* clip rectangle [x0,x1) x [y0,y1) */
static bool ok;

const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
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
    gfx_reset_clip();
    ok = true;
    kprintf("[gfx] framebuffer %dx%dx%d at %p\n", gfx_width, gfx_height, fb_bpp, (uint32_t)fb);
    return true;
}

bool gfx_available(void) { return ok; }

void gfx_set_clip(int x, int y, int w, int h) {
    clip_x0 = MAX(x, 0); clip_y0 = MAX(y, 0);
    clip_x1 = MIN(x + w, gfx_width); clip_y1 = MIN(y + h, gfx_height);
}

void gfx_reset_clip(void) { gfx_set_clip(0, 0, gfx_width, gfx_height); }

void gfx_pixel(int x, int y, uint32_t c) {
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1)
        back[y * gfx_width + x] = c;
}

void gfx_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = MAX(x, clip_x0), y0 = MAX(y, clip_y0);
    int x1 = MIN(x + w, clip_x1), y1 = MIN(y + h, clip_y1);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = back + yy * gfx_width;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void gfx_rect(int x, int y, int w, int h, uint32_t c) {
    gfx_fill(x, y, w, 1, c);
    gfx_fill(x, y + h - 1, w, 1, c);
    gfx_fill(x, y, 1, h, c);
    gfx_fill(x + w - 1, y, 1, h, c);
}

/* Classic "Windows 9x" style 3D border. */
void gfx_bevel(int x, int y, int w, int h, bool raised) {
    uint32_t hi = raised ? 0xFFFFFF : 0x404040, lo = raised ? 0x404040 : 0xFFFFFF;
    uint32_t hi2 = raised ? 0xDFDFDF : 0x808080, lo2 = raised ? 0x808080 : 0xDFDFDF;
    gfx_fill(x, y, w, 1, hi);          gfx_fill(x, y, 1, h, hi);
    gfx_fill(x, y + h - 1, w, 1, lo);  gfx_fill(x + w - 1, y, 1, h, lo);
    gfx_fill(x + 1, y + 1, w - 2, 1, hi2);         gfx_fill(x + 1, y + 1, 1, h - 2, hi2);
    gfx_fill(x + 1, y + h - 2, w - 2, 1, lo2);     gfx_fill(x + w - 2, y + 1, 1, h - 2, lo2);
}

void gfx_char(int x, int y, char ch, uint32_t fg, uint32_t bg, bool transparent) {
    uint8_t c = (uint8_t)ch;
    const uint8_t *g = font8x16[c < 128 ? c : '?'];
    if (x >= clip_x0 && y >= clip_y0 && x + FONT_W <= clip_x1 && y + FONT_H <= clip_y1) {
        for (int r = 0; r < FONT_H; r++) {      /* fast path: fully visible */
            uint32_t *row = back + (y + r) * gfx_width + x;
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

void gfx_gradient_v(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int i = 0; i < h; i++) {
        int t = h > 1 ? (i * 256) / (h - 1) : 0;
        uint32_t r = (((top >> 16) & 0xFF) * (256 - t) + ((bottom >> 16) & 0xFF) * t) >> 8;
        uint32_t g = (((top >> 8) & 0xFF) * (256 - t) + ((bottom >> 8) & 0xFF) * t) >> 8;
        uint32_t b = ((top & 0xFF) * (256 - t) + (bottom & 0xFF) * t) >> 8;
        gfx_fill(x, y + i, w, 1, RGB(r, g, b));
    }
}

void gfx_scroll_region(int y, int h, int pixels, uint32_t fill) {
    if (pixels >= h) { gfx_fill(0, y, gfx_width, h, fill); return; }
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
