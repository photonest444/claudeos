/* Window manager, widget toolkit, themes and animations.
 *
 * Everything GUI-related runs in one kernel task (gui_task). Interrupt
 * handlers only queue input; the task drains the queues, updates state and,
 * when something changed (or an animation is running), composes a frame:
 *   wallpaper -> desktop icons -> windows (each rendered into its own surface,
 *   then blended/scaled onto the screen) -> taskbar -> launcher -> toast ->
 *   mouse cursor, and finally copies the frame to video memory.
 * The same task repaints the CLI console after a CLI/GUI switch. */
#include "../include/gui.h"
#include "../include/gfx.h"
#include "../include/drivers.h"
#include "../include/task.h"
#include "../include/fs.h"
#include "../include/mm.h"
#include "../include/io.h"
#include "../include/string.h"
#include "../include/kprintf.h"

/* ===== themes and settings ================================================ */
static const theme_t themes[2] = {
    /* window    title     text      dim       border    button    btn_hov   btn_prs
       input     in_bord   panel     menu      hover     card */
    { 0x24262B, 0x1C1E22, 0xE8EAED, 0x9AA0A6, 0x383B41, 0x34373D, 0x40444B, 0x2B2E33,
      0x1A1C20, 0x464A51, 0x141518, 0x1F2125, 0x363940, 0x2C2F35 },     /* dark */
    { 0xF8F9FB, 0xECEEF2, 0x1F2328, 0x6B7280, 0xD3D7DE, 0xE6E8EC, 0xDADDE3, 0xCBD0D7,
      0xFFFFFF, 0xC5CAD3, 0xF4F5F7, 0xFFFFFF, 0xE6E9EE, 0xEFF1F4 },     /* light */
};

const uint32_t accent_colors[NUM_ACCENTS] = { 0x3B82F6, 0x8B5CF6, 0x10B981, 0xF59E0B, 0xEC4899, 0x14B8A6 };
const char *const accent_names[NUM_ACCENTS] = { "Blue", "Violet", "Green", "Amber", "Pink", "Teal" };
const char *const wallpaper_names[NUM_WALLPAPERS] = { "Aurora", "Sunset", "Ocean", "Forest", "Graphite" };
static const uint32_t wallpapers[NUM_WALLPAPERS][3] = {     /* gradient start, end, glow */
    { 0x0F2027, 0x2C5364, 0x3FD5C8 },
    { 0x2B1B4D, 0xC0546A, 0xFFB27A },
    { 0x14236B, 0x1FA2C9, 0x9AF0FF },
    { 0x0B3329, 0x5E9C72, 0xCBF2B4 },
    { 0x1A1B1F, 0x3A3F47, 0x8892A0 },
};

gui_settings_t gui_settings;
const theme_t *gui_theme(void) { return &themes[gui_settings.theme ? 1 : 0]; }
uint32_t gui_accent(void) { return accent_colors[gui_settings.accent % NUM_ACCENTS]; }

void gui_wallpaper_colors(int i, uint32_t *c1, uint32_t *c2) {
    *c1 = wallpapers[i % NUM_WALLPAPERS][0];
    *c2 = wallpapers[i % NUM_WALLPAPERS][1];
}

void gui_settings_defaults(void) {
    gui_settings = (gui_settings_t){ .theme = 0, .accent = 0, .wallpaper = 0, .animations = true,
                                     .anim_speed = 1, .shadows = true, .translucent = true,
                                     .seconds = true, .mouse_speed = 100 };
}

#define SETTINGS_FILE "/etc/gui.cfg"

static void settings_load(void) {
    char buf[512];
    gui_settings_defaults();
    int n = fs_read(SETTINGS_FILE, 0, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            int v = atoi(eq + 1);
            if (!strcmp(line, "theme")) gui_settings.theme = v & 1;
            else if (!strcmp(line, "accent")) gui_settings.accent = v % NUM_ACCENTS;
            else if (!strcmp(line, "wallpaper")) gui_settings.wallpaper = v % NUM_WALLPAPERS;
            else if (!strcmp(line, "animations")) gui_settings.animations = v;
            else if (!strcmp(line, "anim_speed")) gui_settings.anim_speed = MIN(MAX(v, 0), 2);
            else if (!strcmp(line, "shadows")) gui_settings.shadows = v;
            else if (!strcmp(line, "translucent")) gui_settings.translucent = v;
            else if (!strcmp(line, "seconds")) gui_settings.seconds = v;
            else if (!strcmp(line, "mouse_speed")) gui_settings.mouse_speed = MIN(MAX(v, 25), 400);
        }
        if (!end) break;
        line = end + 1;
    }
}

static void settings_save(void) {
    char buf[256];
    gui_settings_t *s = &gui_settings;
    int n = snprintf(buf, sizeof(buf),
                     "theme=%d\naccent=%d\nwallpaper=%d\nanimations=%d\nanim_speed=%d\n"
                     "shadows=%d\ntranslucent=%d\nseconds=%d\nmouse_speed=%d\n",
                     s->theme, s->accent, s->wallpaper, s->animations, s->anim_speed,
                     s->shadows, s->translucent, s->seconds, s->mouse_speed);
    if (fs_write(SETTINGS_FILE, buf, n, false) < 0) gui_toast("Could not save settings to " SETTINGS_FILE);
}

/* ===== state ================================================================ */
static window_t  windows[MAX_WINDOWS];
static window_t *zorder[MAX_WINDOWS];      /* bottom ... top */
static int       nz;
static window_t *focused, *hovered;
static volatile int dirty = 1;
static bool      started;
static int       mouse_x, mouse_y, mouse_btn;
static window_t *drag_win;
static int       drag_dx, drag_dy;
static window_t *press_win;
static widget_t *press_widget;
static surface_t *wall_surf;
static int       wall_rendered = -1;
static surface_t *launcher_surf;
static bool      launcher_open;
static uint32_t  launcher_t;               /* start of the last open/close animation */
static uint32_t  fade_t;                   /* start of the desktop fade-in */
static int       last_clock = -1, last_blink = -1;
static volatile int requested_app = -1;
static volatile bool apply_request;
static struct { char text[96]; uint32_t start; bool active; } toast;

#define KEYQ 64
static int keyq[KEYQ];
static volatile uint32_t kq_head, kq_tail;

void gui_invalidate(void) { dirty = 1; }

void gui_post_key(int key) {               /* called from the keyboard IRQ */
    uint32_t next = (kq_head + 1) % KEYQ;
    if (next != kq_tail) { keyq[kq_head] = key; kq_head = next; }
}

void gui_request_app(int app) { requested_app = app; }
void gui_request_settings_apply(void) { apply_request = true; }
bool gui_is_focused(window_t *w) { return w == focused; }
bool gui_blink_on(void) { return (timer_ticks() / 530) % 2 == 0; }

void gui_toast(const char *msg) {
    strlcpy(toast.text, msg, sizeof(toast.text));
    toast.start = timer_ticks();
    toast.active = true;
    dirty = 1;
}

/* ===== animation helpers ===================================================== */
enum { ANIM_NONE, ANIM_OPEN, ANIM_CLOSE, ANIM_MIN, ANIM_RESTORE };
static const int anim_ms[] = { 0, 230, 170, 260, 260 };
#define LAUNCHER_MS 190
#define FADE_MS     350
#define TOAST_IN    260
#define TOAST_OUT   350
#define TOAST_TOTAL 2800

static uint32_t now(void) { return timer_ticks(); }

static int duration(int base) {
    static const int pct[3] = { 170, 100, 55 };
    return gui_settings.animations ? base * pct[gui_settings.anim_speed % 3] / 100 : 0;
}

static int progress(uint32_t start, int d) {     /* 0..1000 */
    if (d <= 0) return 1000;
    uint32_t e = now() - start;
    return e >= (uint32_t)d ? 1000 : (int)(e * 1000 / (uint32_t)d);
}

static int ease_out(int t) {                     /* cubic ease-out, 0..1000 */
    int u = 1000 - t;
    return 1000 - (u * u / 1000) * u / 1000;
}

static int lerp(int a, int b, int t) { return a + (b - a) * t / 1000; }

/* ===== app registry and icons ================================================ */
typedef struct { const char *name; void (*open)(void); uint32_t c1, c2; bool single; } app_info_t;
static const app_info_t apps[APP_COUNT] = {
    [APP_TERMINAL] = { "Terminal",   app_terminal,     0x3A4150, 0x15181E, false },
    [APP_FILES]    = { "Files",      app_files,        0xFCC54D, 0xF08C1A, false },
    [APP_CALC]     = { "Calculator", app_calculator,   0x6AA8FF, 0x2F5FE0, false },
    [APP_MONITOR]  = { "Monitor",    app_sysmon,       0x4BD8A0, 0x0C8C63, true },
    [APP_WIDGETS]  = { "Widgets",    app_widgets_demo, 0xF784C4, 0xD02A7C, false },
    [APP_SETTINGS] = { "Settings",   app_settings,     0xA4ACB9, 0x4D5563, true },
    [APP_ABOUT]    = { "About",      app_about,        0xB49BFF, 0x6D3FE6, true },
};
#define DESKTOP_APPS 6

/* The ClaudeOS logo: a ring with a small moon. */
void gui_draw_logo(int cx, int cy, int r, uint32_t color) {
    gfx_ring(cx, cy, r, r - MAX(2, r * 2 / 5), color, 255);
    int mr = MAX(2, r * 3 / 10), off = r * 72 / 100;
    gfx_circle(cx + off, cy - off, mr, color, 255);
}

void gui_draw_app_icon(int app, int x, int y, int s) {
    const app_info_t *a = &apps[app];
    gfx_round_gradient(x, y, s, s, s / 4, a->c1, a->c2);
    uint32_t w = 0xFFFFFF;
    switch (app) {
    case APP_TERMINAL:
        if (s >= 40) gfx_text_scaled(x + s / 2 - 16, y + s / 2 - 16, ">_", w, 2);
        else gfx_text_bold(x + s / 2 - 8, y + s / 2 - 8, ">_", w);
        break;
    case APP_FILES:
        gfx_round_fill(x + s * 20 / 100, y + s * 28 / 100, s * 28 / 100, s * 14 / 100, MAX(1, s / 20), 0xFFF3D6);
        gfx_round_fill(x + s * 20 / 100, y + s * 36 / 100, s * 60 / 100, s * 40 / 100, MAX(1, s / 12), w);
        break;
    case APP_CALC: {
        int q = s * 22 / 100, gap = s * 8 / 100, ox = x + (s - 2 * q - gap) / 2, oy = y + (s - 2 * q - gap) / 2;
        for (int i = 0; i < 4; i++)
            gfx_round_fill(ox + (i % 2) * (q + gap), oy + (i / 2) * (q + gap), q, q, MAX(1, q / 4),
                           i == 3 ? 0xFFD089 : w);
        break;
    }
    case APP_MONITOR: {
        static const int hts[3] = { 30, 52, 40 };
        int bw = s * 13 / 100, gap = s * 8 / 100, ox = x + (s - 3 * bw - 2 * gap) / 2, base = y + s * 76 / 100;
        for (int i = 0; i < 3; i++)
            gfx_round_fill(ox + i * (bw + gap), base - s * hts[i] / 100, bw, s * hts[i] / 100, MAX(1, bw / 3), w);
        break;
    }
    case APP_WIDGETS: {
        int pw = s * 60 / 100, ph = s * 30 / 100, px = x + (s - pw) / 2, py = y + (s - ph) / 2;
        gfx_round_fill(px, py, pw, ph, ph / 2, w);
        gfx_circle(px + pw - ph / 2, py + ph / 2, ph / 2 - MAX(1, s / 20), a->c2, 255);
        break;
    }
    case APP_SETTINGS: {
        int cx = x + s / 2, cy = y + s / 2, r = s * 24 / 100, tooth = MAX(2, s * 12 / 100);
        static const int dir[8][2] = { {1000,0},{707,707},{0,1000},{-707,707},{-1000,0},{-707,-707},{0,-1000},{707,-707} };
        for (int i = 0; i < 8; i++)
            gfx_circle(cx + dir[i][0] * (r + tooth / 3) / 1000, cy + dir[i][1] * (r + tooth / 3) / 1000, tooth / 2 + 1, w, 255);
        gfx_circle(cx, cy, r, w, 255);
        gfx_circle(cx, cy, MAX(1, r * 45 / 100), a->c2, 255);
        break;
    }
    case APP_ABOUT:
        gui_draw_logo(x + s / 2 - s / 20, y + s / 2 + s / 20, s * 26 / 100, w);
        break;
    }
}

/* ===== window management ====================================================== */
static void raise_window(window_t *w) {
    int i;
    for (i = 0; i < nz && zorder[i] != w; i++) ;
    if (i == nz) return;
    for (; i < nz - 1; i++) zorder[i] = zorder[i + 1];
    zorder[nz - 1] = w;
    focused = w;
    dirty = 1;
}

static void focus_topmost(void) {
    focused = NULL;
    for (int i = nz - 1; i >= 0; i--)
        if (!zorder[i]->minimized && !zorder[i]->closing) { focused = zorder[i]; break; }
}

static void start_anim(window_t *w, int kind) {
    w->anim = kind;
    w->anim_start = now();
    dirty = 1;
}

window_t *gui_window_create(int app, const char *title, int x, int y, int cw, int ch) {
    static int cascade;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (w->used) continue;
        memset(w, 0, sizeof(*w));
        w->w = cw;
        w->h = ch + TITLE_H;
        w->surf = gfx_surface_new(w->w, w->h);
        if (!w->surf) { gui_toast("Not enough memory for a new window"); return NULL; }
        if (x < 0) {                                    /* cascade new windows */
            x = 150 + cascade * 34;
            y = 36 + cascade * 30;
            cascade = (cascade + 1) % 7;
        }
        w->used = true;
        w->app = app;
        w->x = MAX(0, MIN(x, gfx_width - w->w));
        w->y = MAX(0, MIN(y, gfx_height - TASKBAR_H - w->h));
        w->focus = -1;
        strlcpy(w->title, title, sizeof(w->title));
        zorder[nz++] = w;
        focused = w;
        start_anim(w, ANIM_OPEN);
        return w;
    }
    gui_toast("Too many windows open");
    return NULL;
}

static void window_finalize(window_t *w) {           /* after the close animation */
    gfx_surface_free(w->surf);
    w->surf = NULL;
    int i;
    for (i = 0; i < nz && zorder[i] != w; i++) ;
    if (i < nz) {
        for (; i < nz - 1; i++) zorder[i] = zorder[i + 1];
        nz--;
    }
    w->used = false;
    w->closing = false;
    w->anim = ANIM_NONE;
    dirty = 1;
}

void gui_window_close(window_t *w) {
    if (!w->used || w->closing) return;
    if (w->on_close) w->on_close(w);
    for (int i = 0; i < w->nwidgets; i++) kfree(w->widgets[i]);
    w->nwidgets = 0;
    if (w->data && w->free_data) kfree(w->data);
    w->data = NULL;
    w->closing = true;
    if (drag_win == w) drag_win = NULL;
    if (press_win == w) { press_win = NULL; press_widget = NULL; }
    if (focused == w) focus_topmost();
    start_anim(w, ANIM_CLOSE);
    if (!duration(anim_ms[ANIM_CLOSE])) window_finalize(w);
}

static void minimize_window(window_t *w) {
    w->minimized = true;
    if (focused == w) focus_topmost();
    start_anim(w, ANIM_MIN);
}

static void restore_window(window_t *w) {
    w->minimized = false;
    raise_window(w);
    start_anim(w, ANIM_RESTORE);
}

widget_t *gui_add_widget(window_t *w, int type, int x, int y, int ww, int wh, const char *text) {
    if (!w || w->nwidgets >= MAX_WIDGETS) return NULL;
    widget_t *wd = kzalloc(sizeof(widget_t));
    if (!wd) return NULL;
    wd->type = type;
    wd->x = x; wd->y = y; wd->w = ww; wd->h = wh;
    wd->max = 100;
    strlcpy(wd->text, text ? text : "", sizeof(wd->text));
    wd->cursor = strlen(wd->text);
    w->widgets[w->nwidgets++] = wd;
    if (type == W_TEXTBOX && w->focus < 0) w->focus = w->nwidgets - 1;
    dirty = 1;
    return wd;
}

void gui_client_size(window_t *w, int *cw, int *ch) { *cw = w->w; *ch = w->h - TITLE_H; }

void gui_open_app(int app) {
    if (app < 0 || app >= APP_COUNT) return;
    if (apps[app].single)                              /* bring the existing one forward */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t *w = &windows[i];
            if (w->used && !w->closing && w->app == app) {
                if (w->minimized) restore_window(w); else raise_window(w);
                return;
            }
        }
    apps[app].open();
}

/* ===== geometry =============================================================== */
static bool inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

#define PILL_X0 58
#define PILL_W  180
#define PILL_GAP 6
#define CLOCK_W 128

/* Taskbar buttons share the free space; they shrink (down to icon-only) when
 * many windows are open. */
static bool pill_rect(window_t *target, int *x, int *y, int *w, int *h) {
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) count += windows[i].used && !windows[i].closing;
    if (!count) return false;
    int avail = gfx_width - CLOCK_W - PILL_X0 - 8;
    int pw = MIN(PILL_W, avail / count - PILL_GAP);
    if (pw < 36) pw = 36;
    int px = PILL_X0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w2 = &windows[i];
        if (!w2->used || w2->closing) continue;
        if (px + pw > gfx_width - CLOCK_W) return false;
        if (w2 == target) { *x = px; *y = gfx_height - TASKBAR_H + 6; *w = pw; *h = 36; return true; }
        px += pw + PILL_GAP;
    }
    return false;
}

/* Where (and how transparent) a window is drawn, taking animations into account. */
static bool window_view(window_t *w, int *x, int *y, int *ww, int *hh, int *alpha) {
    *x = w->x; *y = w->y; *ww = w->w; *hh = w->h; *alpha = 255;
    if (w->minimized && w->anim != ANIM_MIN) return false;
    if (!w->anim) return true;
    int t = progress(w->anim_start, duration(anim_ms[w->anim])), e = ease_out(t), scale = 1000;
    switch (w->anim) {
    case ANIM_OPEN:  scale = 920 + 80 * e / 1000;  *alpha = 255 * e / 1000; break;
    case ANIM_CLOSE: scale = 1000 - 70 * e / 1000; *alpha = 255 - 255 * e / 1000; break;
    case ANIM_MIN: case ANIM_RESTORE: {
        int px, py, pw, ph, k = w->anim == ANIM_MIN ? e : 1000 - e;
        if (!pill_rect(w, &px, &py, &pw, &ph)) { px = gfx_width / 2; py = gfx_height; pw = 40; ph = 20; }
        *x = lerp(w->x, px, k); *y = lerp(w->y, py, k);
        *ww = MAX(8, lerp(w->w, pw, k)); *hh = MAX(8, lerp(w->h, ph, k));
        *alpha = 255 - 215 * k / 1000;
        return true;
    }
    }
    *ww = w->w * scale / 1000; *hh = w->h * scale / 1000;
    *x = w->x + (w->w - *ww) / 2; *y = w->y + (w->h - *hh) / 2;
    return true;
}

static window_t *window_at(int x, int y) {
    for (int i = nz - 1; i >= 0; i--) {
        window_t *w = zorder[i];
        if (!w->minimized && !w->closing && inside(x, y, w->x, w->y, w->w, w->h)) return w;
    }
    return NULL;
}

bool gui_mouse_in_client(window_t *w, int *x, int *y) {
    *x = mouse_x - w->x;
    *y = mouse_y - w->y - TITLE_H;
    return hovered == w && *y >= 0;
}

#define LAUNCHER_W 300
#define LAUNCHER_ITEM_Y 76
#define LAUNCHER_ITEM_H 40
#define LAUNCHER_FOOT (LAUNCHER_ITEM_Y + APP_COUNT * LAUNCHER_ITEM_H + 8)
#define LAUNCHER_H (LAUNCHER_FOOT + 54)
static int launcher_x(void) { return 8; }
static int launcher_y(void) { return gfx_height - TASKBAR_H - 8 - LAUNCHER_H; }

typedef struct { const char *label; int x, w; } foot_btn_t;
static const foot_btn_t foot_btns[3] = { { "Console", 12, 84 }, { "Restart", 102, 84 }, { "Power off", 192, 96 } };

/* ===== drawing: widgets ======================================================= */
static int nsegments(const char *t) { int n = 1; for (; *t; t++) n += *t == '|'; return n; }

static void segment(const char *t, int idx, char *out, int max) {
    for (int i = 0; i < idx && *t; t++) if (*t == '|') i++;
    int n = 0;
    while (*t && *t != '|' && n < max - 1) out[n++] = *t++;
    out[n] = 0;
}

static void text_center(int x, int y, int w, int h, const char *s, uint32_t c, bool bold) {
    int tx = x + (w - (int)strlen(s) * FONT_W) / 2, ty = y + (h - FONT_H) / 2;
    if (bold) gfx_text_bold(tx, ty, s, c); else gfx_text(tx, ty, s, c);
}

static void focus_ring(int x, int y, int w, int h, int r) {
    gfx_round_outline(x - 2, y - 2, w + 4, h + 4, r + 2, gui_accent(), 255);
    gfx_round_outline(x - 3, y - 3, w + 6, h + 6, r + 3, gui_accent(), 110);
}

static void draw_widget(widget_t *wd, int ox, int oy, bool focus, bool hover) {
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    int x = ox + wd->x, y = oy + wd->y;
    switch (wd->type) {
    case W_LABEL: {
        uint32_t c = wd->color ? wd->color : wd->style == S_DIM ? th->dim : wd->style == S_ACCENT ? acc : th->text;
        if (wd->style == S_BIG) gfx_text_scaled(x, y + (wd->h - 32) / 2, wd->text, c, 2);
        else if (wd->style == S_HEADING) gfx_text_bold(x, y + (wd->h - FONT_H) / 2, wd->text, c);
        else gfx_text(x, y + (wd->h - FONT_H) / 2, wd->text, c);
        break;
    }
    case W_BUTTON: {
        uint32_t bg, fg = th->text;
        if (wd->style == S_PRIMARY) {
            bg = wd->pressed ? gfx_mix(acc, 0, 50) : hover ? gfx_mix(acc, 0xFFFFFF, 36) : acc;
            fg = 0xFFFFFF;
        } else {
            bg = wd->pressed ? th->button_press : hover ? th->button_hover : th->button;
            if (wd->style == S_DANGER) fg = 0xEF5350;
            if (wd->style == S_ACCENT) fg = acc;
        }
        gfx_round_fill(x, y, wd->w, wd->h, 8, bg);
        text_center(x, y + (wd->pressed ? 1 : 0), wd->w, wd->h, wd->text, fg, wd->style == S_PRIMARY);
        if (focus) focus_ring(x, y, wd->w, wd->h, 8);
        break;
    }
    case W_TEXTBOX: {
        gfx_round_fill(x, y, wd->w, wd->h, 8, th->input);
        gfx_round_outline(x, y, wd->w, wd->h, 8, focus ? acc : hover ? th->dim : th->input_border, 255);
        if (focus) gfx_round_outline(x + 1, y + 1, wd->w - 2, wd->h - 2, 7, acc, 255);
        int visible = (wd->w - 20) / FONT_W, ty = y + (wd->h - FONT_H) / 2;
        int start = wd->cursor > visible - 1 ? wd->cursor - (visible - 1) : 0;
        char buf[96];
        strlcpy(buf, wd->text + start, MIN((int)sizeof(buf), visible + 1));
        if (!wd->text[0] && wd->placeholder[0]) gfx_text(x + 10, ty, wd->placeholder, th->dim);
        else gfx_text(x + 10, ty, buf, th->text);
        if (focus && gui_blink_on()) gfx_fill(x + 10 + (wd->cursor - start) * FONT_W, ty - 1, 2, FONT_H + 2, acc);
        break;
    }
    case W_TOGGLE: {
        int t = wd->anim, kd = wd->h - 6;
        gfx_round_fill(x, y, wd->w, wd->h, wd->h / 2, gfx_mix(th->button_hover, acc, t * 255 / 1000));
        int kx = x + 3 + (wd->w - kd - 6) * t / 1000;
        gfx_circle(kx + kd / 2, y + 3 + kd / 2, kd / 2, 0xFFFFFF, 255);
        if (focus) focus_ring(x, y, wd->w, wd->h, wd->h / 2);
        break;
    }
    case W_SLIDER: {
        int range = MAX(1, wd->max - wd->min), pos = (wd->value - wd->min) * (wd->w - 18) / range, cy = y + wd->h / 2;
        gfx_round_fill(x, cy - 3, wd->w, 6, 3, th->button_hover);
        gfx_round_fill(x, cy - 3, pos + 9, 6, 3, acc);
        int kr = (hover || wd->pressed) ? 10 : 9;
        gfx_circle(x + 9 + pos, cy, kr, acc, 255);
        gfx_circle(x + 9 + pos, cy, kr - 3, 0xFFFFFF, 255);
        if (focus) focus_ring(x, y, wd->w, wd->h, 6);
        break;
    }
    case W_CHOICE: {
        int n = nsegments(wd->text), sw = wd->w / n;
        gfx_round_fill(x, y, wd->w, wd->h, 9, th->input);
        gfx_round_outline(x, y, wd->w, wd->h, 9, th->input_border, 255);
        gfx_round_fill(x + 3 + wd->anim * sw / 1000, y + 3, sw - 6, wd->h - 6, 7, acc);
        for (int i = 0; i < n; i++) {
            char seg[24];
            segment(wd->text, i, seg, sizeof(seg));
            text_center(x + i * sw, y, sw, wd->h, seg, i == wd->value ? 0xFFFFFF : th->text, i == wd->value);
        }
        if (focus) focus_ring(x, y, wd->w, wd->h, 9);
        break;
    }
    case W_SWATCH:
        if (wd->style == S_WALLPAPER) {
            uint32_t c1, c2;
            gui_wallpaper_colors(wd->tag, &c1, &c2);
            gfx_round_gradient(x, y, wd->w, wd->h, 8, c1, c2);
            gfx_circle(x + wd->w * 3 / 4, y + wd->h / 3, wd->h / 5, wallpapers[wd->tag % NUM_WALLPAPERS][2], 120);
            if (wd->value || hover) {
                gfx_round_outline(x - 3, y - 3, wd->w + 6, wd->h + 6, 11, wd->value ? acc : th->dim, 255);
                if (wd->value) gfx_round_outline(x - 2, y - 2, wd->w + 4, wd->h + 4, 10, acc, 255);
            }
        } else {
            int r = MIN(wd->w, wd->h) / 2;
            gfx_circle(x + r, y + r, r - 4, wd->color, 255);
            if (wd->value || hover) {
                gfx_round_outline(x, y, 2 * r, 2 * r, r, wd->value ? wd->color : th->dim, 255);
                gfx_round_outline(x + 1, y + 1, 2 * r - 2, 2 * r - 2, r - 1, wd->value ? wd->color : th->dim, 160);
            }
        }
        if (focus) focus_ring(x, y, wd->w, wd->h, 8);
        break;
    }
}

/* ===== drawing: windows ======================================================= */
static void draw_cross(int cx, int cy, int r, uint32_t c) {
    for (int i = -r; i <= r; i++) {
        gfx_pixel(cx + i, cy + i, c); gfx_pixel(cx + i + 1, cy + i, c);
        gfx_pixel(cx + i, cy - i, c); gfx_pixel(cx + i + 1, cy - i, c);
    }
}

static void render_window(window_t *w) {
    const theme_t *th = gui_theme();
    bool active = w == focused;
    gfx_set_target(w->surf);
    gfx_fill(0, 0, w->w, w->h, th->window);
    gfx_fill(0, 0, w->w, TITLE_H, th->title);
    gfx_fill(0, TITLE_H - 1, w->w, 1, th->border);
    gui_draw_app_icon(w->app, 12, (TITLE_H - 20) / 2, 20);
    gfx_set_clip(40, 0, w->w - 130, TITLE_H);
    gfx_text_bold(42, (TITLE_H - FONT_H) / 2, w->title, active ? th->text : th->dim);
    gfx_reset_clip();

    int mx = mouse_x - w->x, my = mouse_y - w->y;
    bool over = hovered == w && !drag_win;
    int cbx = w->w - 44, mbx = w->w - 82, by = 6;
    if (over && inside(mx, my, cbx, by, 36, 24)) {
        gfx_round_fill(cbx, by, 36, 24, 7, 0xE5484D);
        draw_cross(cbx + 17, by + 12, 4, 0xFFFFFF);
    } else draw_cross(cbx + 17, by + 12, 4, active ? th->text : th->dim);
    if (over && inside(mx, my, mbx, by, 36, 24)) gfx_round_fill(mbx, by, 36, 24, 7, th->hover);
    gfx_fill(mbx + 12, by + 12, 12, 2, active ? th->text : th->dim);

    gfx_set_clip(0, TITLE_H, w->w, w->h - TITLE_H);
    for (int i = 0; i < w->nwidgets; i++) {
        widget_t *wd = w->widgets[i];
        bool hov = over && inside(mx, my - TITLE_H, wd->x, wd->y, wd->w, wd->h);
        draw_widget(wd, 0, TITLE_H, active && w->focus == i, hov);
    }
    if (w->paint) w->paint(w, 0, TITLE_H);
    gfx_reset_clip();
    gfx_round_outline(0, 0, w->w, w->h, WIN_RADIUS, active ? gfx_mix(th->border, gui_accent(), 90) : th->border, 255);
    gfx_surface_round(w->surf, WIN_RADIUS);
    gfx_set_target(NULL);
}

/* ===== drawing: desktop, taskbar, launcher, toast ============================= */
static void render_wallpaper(void) {
    if (!wall_surf) wall_surf = gfx_surface_new(gfx_width, gfx_height);
    if (!wall_surf) return;
    const uint32_t *wp = wallpapers[gui_settings.wallpaper % NUM_WALLPAPERS];
    int W = gfx_width, H = gfx_height;
    struct { int x, y, r, a; uint32_t c; } blobs[3] = {
        { W * 80 / 100, H * 22 / 100, H * 62 / 100, 120, wp[2] },
        { W * 12 / 100, H * 90 / 100, H * 55 / 100, 70, wp[1] },
        { W * 45 / 100, H * 60 / 100, H * 40 / 100, 40, wp[2] },
    };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t c = gfx_mix(wp[0], wp[1], (x + y) * 255 / (W + H));
            for (int b = 0; b < 3; b++) {
                int dx = x - blobs[b].x, dy = y - blobs[b].y;
                uint32_t d2 = (uint32_t)(dx * dx + dy * dy), r2 = (uint32_t)(blobs[b].r * blobs[b].r);
                if (d2 >= r2) continue;
                int f = 1000 - (int)(d2 * 1000 / r2);           /* 1000 at the centre */
                c = gfx_mix(c, blobs[b].c, blobs[b].a * f / 1000 * f / 1000);
            }
            wall_surf->px[y * W + x] = c | 0xFF000000u;
        }
    wall_rendered = gui_settings.wallpaper;
}

static void draw_desktop(void) {
    if (wall_surf) gfx_blit_opaque(wall_surf, 0, 0);
    else gfx_fill(0, 0, gfx_width, gfx_height, 0x1E2A38);
    for (int i = 0; i < DESKTOP_APPS; i++) {
        int x = 16, y = 16 + i * 96;
        if (!window_at(mouse_x, mouse_y) && inside(mouse_x, mouse_y, x, y, 80, 88) && mouse_y < gfx_height - TASKBAR_H)
            gfx_round_blend(x, y, 80, 88, 12, 0xFFFFFF, 38);
        gfx_round_blend(x + 13, y + 9, 56, 56, 14, 0, 50);          /* soft icon shadow */
        gui_draw_app_icon(i, x + 12, y + 6, 56);
        int tx = x + (80 - (int)strlen(apps[i].name) * FONT_W) / 2;
        gfx_text(tx + 1, y + 67, apps[i].name, 0x000000);
        gfx_text(tx, y + 66, apps[i].name, 0xFFFFFF);
    }
}

static int weekday(int y, int m, int d) {             /* 0 = Sunday */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static void draw_taskbar(void) {
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    int y0 = gfx_height - TASKBAR_H;
    gfx_blend(0, y0, gfx_width, TASKBAR_H, th->panel, gui_settings.translucent ? 205 : 255);
    gfx_blend(0, y0, gfx_width, 1, gui_settings.theme ? 0x000000 : 0xFFFFFF, 28);

    bool lhover = inside(mouse_x, mouse_y, 8, y0 + 4, 40, 40);
    if (lhover || launcher_open) gfx_round_blend(8, y0 + 4, 40, 40, 10, th->hover, 230);
    gui_draw_logo(28, y0 + 24, 11, acc);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        int px, py, pw, ph;
        if (!w->used || w->closing || !pill_rect(w, &px, &py, &pw, &ph)) continue;
        bool active = w == focused && !w->minimized, hov = inside(mouse_x, mouse_y, px, py, pw, ph);
        if (active) gfx_round_blend(px, py, pw, ph, 9, th->hover, 255);
        else if (hov) gfx_round_blend(px, py, pw, ph, 9, th->hover, 150);
        if (pw < 80) gui_draw_app_icon(w->app, px + (pw - 20) / 2, py + 8, 20);
        else {
            gui_draw_app_icon(w->app, px + 8, py + 8, 20);
            gfx_set_clip(px + 34, py, pw - 40, ph);
            gfx_text(px + 36, py + 10, w->title, w->minimized ? th->dim : th->text);
            gfx_reset_clip();
        }
        int bar = active ? 18 : 6;
        gfx_round_fill(px + (pw - bar) / 2, py + ph - 4, bar, 3, 1, w->minimized ? th->dim : acc);
    }

    rtc_time_t t;
    rtc_read(&t);
    char tbuf[16], dbuf[24];
    static const char *days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mons[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (gui_settings.seconds) snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", t.hour, t.min, t.sec);
    else snprintf(tbuf, sizeof(tbuf), "%02d:%02d", t.hour, t.min);
    snprintf(dbuf, sizeof(dbuf), "%s %d %s", days[weekday(t.year, t.month, t.day)], t.day, mons[(t.month + 11) % 12]);
    int cx = gfx_width - CLOCK_W / 2 - 8;
    gfx_text_bold(cx - (int)strlen(tbuf) * FONT_W / 2, y0 + 6, tbuf, th->text);
    gfx_text(cx - (int)strlen(dbuf) * FONT_W / 2, y0 + 25, dbuf, th->dim);
    last_clock = gui_settings.seconds ? t.sec : t.min;
}

static int launcher_item_at(int x, int y) {          /* coordinates relative to the launcher */
    if (y >= LAUNCHER_ITEM_Y && y < LAUNCHER_ITEM_Y + APP_COUNT * LAUNCHER_ITEM_H && x >= 8 && x < LAUNCHER_W - 8)
        return (y - LAUNCHER_ITEM_Y) / LAUNCHER_ITEM_H;
    if (y >= LAUNCHER_FOOT + 10 && y < LAUNCHER_FOOT + 42)
        for (int i = 0; i < 3; i++)
            if (x >= foot_btns[i].x && x < foot_btns[i].x + foot_btns[i].w) return 100 + i;
    return -1;
}

static void render_launcher(void) {
    if (!launcher_surf) launcher_surf = gfx_surface_new(LAUNCHER_W, LAUNCHER_H);
    if (!launcher_surf) return;
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    int hover = launcher_item_at(mouse_x - launcher_x(), mouse_y - launcher_y());
    gfx_set_target(launcher_surf);
    gfx_fill(0, 0, LAUNCHER_W, LAUNCHER_H, th->menu);
    gfx_round_fill(16, 16, 44, 44, 12, gfx_mix(th->menu, acc, 40));
    gui_draw_logo(37, 39, 12, acc);
    gfx_text_bold(72, 20, "ClaudeOS", th->text);
    gfx_text(72, 39, "root@claudeos", th->dim);
    gfx_fill(16, LAUNCHER_ITEM_Y - 8, LAUNCHER_W - 32, 1, th->border);
    for (int i = 0; i < APP_COUNT; i++) {
        int iy = LAUNCHER_ITEM_Y + i * LAUNCHER_ITEM_H;
        if (hover == i) gfx_round_fill(8, iy, LAUNCHER_W - 16, LAUNCHER_ITEM_H - 4, 9, th->hover);
        gui_draw_app_icon(i, 18, iy + 6, 24);
        gfx_text(54, iy + 10, apps[i].name, th->text);
    }
    gfx_fill(16, LAUNCHER_FOOT, LAUNCHER_W - 32, 1, th->border);
    for (int i = 0; i < 3; i++) {
        const foot_btn_t *b = &foot_btns[i];
        gfx_round_fill(b->x, LAUNCHER_FOOT + 10, b->w, 32, 8, hover == 100 + i ? (i == 2 ? 0xE5484D : th->button_hover) : th->button);
        text_center(b->x, LAUNCHER_FOOT + 10, b->w, 32, b->label, hover == 102 && i == 2 ? 0xFFFFFF : th->text, false);
    }
    gfx_round_outline(0, 0, LAUNCHER_W, LAUNCHER_H, 14, th->border, 255);
    gfx_surface_round(launcher_surf, 14);
    gfx_set_target(NULL);
}

static void set_launcher(bool open) {
    if (open == launcher_open) return;
    launcher_open = open;
    launcher_t = now();
    dirty = 1;
}

static void draw_launcher(void) {
    int t = progress(launcher_t, duration(LAUNCHER_MS));
    if (!launcher_open && t >= 1000) return;
    int e = ease_out(t), vis = launcher_open ? e : 1000 - e;   /* 0 hidden .. 1000 shown */
    render_launcher();
    if (!launcher_surf) return;
    int x = launcher_x(), y = launcher_y() + (1000 - vis) * 18 / 1000;
    int alpha = vis * (gui_settings.translucent ? 245 : 255) / 1000;
    if (gui_settings.shadows) gfx_shadow(x, y, LAUNCHER_W, LAUNCHER_H, 14, 14, 60 * vis / 1000);
    gfx_blit(launcher_surf, x, y, alpha);
}

static void draw_toast(void) {
    if (!toast.active) return;
    uint32_t el = now() - toast.start;
    int total = gui_settings.animations ? TOAST_TOTAL : TOAST_TOTAL - TOAST_IN - TOAST_OUT;
    if (el >= (uint32_t)total) { toast.active = false; return; }
    const theme_t *th = gui_theme();
    int w = (int)strlen(toast.text) * FONT_W + 56, h = 44, slide = 0, alpha = 255;
    if (gui_settings.animations) {
        if (el < TOAST_IN) slide = (1000 - ease_out((int)el * 1000 / TOAST_IN)) * (w + 24) / 1000;
        if (el > (uint32_t)(TOAST_TOTAL - TOAST_OUT)) alpha = 255 * (int)(TOAST_TOTAL - el) / TOAST_OUT;
    }
    int x = gfx_width - w - 16 + slide, y = 16;
    if (gui_settings.shadows) gfx_shadow(x, y, w, h, 12, 10, 60 * alpha / 255);
    gfx_round_blend(x, y, w, h, 12, th->menu, alpha);
    gfx_round_outline(x, y, w, h, 12, th->border, alpha);
    gfx_circle(x + 22, y + h / 2, 5, gui_accent(), alpha);
    if (alpha > 128) gfx_text(x + 38, y + 14, toast.text, th->text);
}

static const char *cursor_shape[] = {
    "X           ", "XX          ", "XoX         ", "XooX        ", "XoooX       ",
    "XooooX      ", "XoooooX     ", "XooooooX    ", "XoooooooX   ", "XooooooooX  ",
    "XoooooXXXXX ", "XooXooX     ", "XoX XooX    ", "XX  XooX    ", "X    XooX   ",
    "     XooX   ", "      XX    ",
};

static void draw_cursor(void) {
    for (int y = 0; y < 17; y++)
        for (int x = 0; cursor_shape[y][x]; x++) {
            char c = cursor_shape[y][x];
            if (c == 'X') gfx_pixel(mouse_x + x, mouse_y + y, 0x101114);
            else if (c == 'o') gfx_pixel(mouse_x + x, mouse_y + y, 0xFFFFFF);
        }
}

static void compose(void) {
    gfx_set_target(NULL);
    hovered = window_at(mouse_x, mouse_y);
    if (launcher_open && inside(mouse_x, mouse_y, launcher_x(), launcher_y(), LAUNCHER_W, LAUNCHER_H)) hovered = NULL;
    draw_desktop();
    for (int i = 0; i < nz; i++) {
        window_t *w = zorder[i];
        int x, y, ww, hh, alpha;
        if (!window_view(w, &x, &y, &ww, &hh, &alpha) || !w->surf) continue;
        if (!w->closing) render_window(w);                   /* closing: keep last image */
        if (gui_settings.shadows) gfx_shadow(x, y, ww, hh, WIN_RADIUS, w == focused ? 16 : 10, (w == focused ? 70 : 45) * alpha / 255);
        if (ww == w->w && hh == w->h) gfx_blit(w->surf, x, y, alpha);
        else gfx_blit_scaled(w->surf, x, y, ww, hh, alpha);
    }
    draw_taskbar();
    draw_launcher();
    draw_toast();
    int ft = progress(fade_t, duration(FADE_MS));
    if (ft < 1000) gfx_blend(0, 0, gfx_width, gfx_height, 0, 255 - 255 * ease_out(ft) / 1000);
    draw_cursor();
    if (display_mode == DISPLAY_GUI) gfx_flush_all();
}

/* ===== input ================================================================== */
static void widget_set_from_mouse(window_t *w, widget_t *wd, int cx) {
    if (wd->type != W_SLIDER) return;
    int range = wd->max - wd->min, v = wd->min + (cx - wd->x - 9) * range / MAX(1, wd->w - 18);
    v = MAX(wd->min, MIN(wd->max, v));
    if (v != wd->value) { wd->value = v; if (wd->on_change) wd->on_change(w, wd); }
}

static void left_down(int x, int y) {
    if (launcher_open) {
        int item = launcher_item_at(x - launcher_x(), y - launcher_y());
        bool on_button = inside(x, y, 8, gfx_height - TASKBAR_H + 4, 40, 40);
        set_launcher(false);
        if (item >= 0 && item < APP_COUNT) { gui_open_app(item); return; }
        if (item == 100) { display_set_mode(DISPLAY_CLI); return; }
        if (item == 101) power_reboot();
        if (item == 102) power_shutdown();
        if (on_button || inside(x, y, launcher_x(), launcher_y(), LAUNCHER_W, LAUNCHER_H)) return;
    }
    if (y >= gfx_height - TASKBAR_H) {                       /* taskbar */
        if (inside(x, y, 8, gfx_height - TASKBAR_H + 4, 40, 40)) { set_launcher(true); return; }
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t *w = &windows[i];
            int px, py, pw, ph;
            if (!w->used || w->closing || !pill_rect(w, &px, &py, &pw, &ph) || !inside(x, y, px, py, pw, ph)) continue;
            if (w->minimized) restore_window(w);
            else if (w == focused) minimize_window(w);
            else raise_window(w);
            return;
        }
        return;
    }
    window_t *w = window_at(x, y);
    if (!w) {                                               /* desktop icons */
        focused = NULL;
        dirty = 1;
        for (int i = 0; i < DESKTOP_APPS; i++)
            if (inside(x, y, 16, 16 + i * 96, 80, 88)) { gui_open_app(i); return; }
        return;
    }
    raise_window(w);
    if (y < w->y + TITLE_H) {                               /* title bar */
        int lx = x - w->x, ly = y - w->y;
        if (inside(lx, ly, w->w - 44, 6, 36, 24)) { gui_window_close(w); return; }
        if (inside(lx, ly, w->w - 82, 6, 36, 24)) { minimize_window(w); return; }
        drag_win = w;
        drag_dx = x - w->x;
        drag_dy = y - w->y;
        return;
    }
    int cx = x - w->x, cy = y - w->y - TITLE_H;
    for (int i = 0; i < w->nwidgets; i++) {
        widget_t *wd = w->widgets[i];
        if (!inside(cx, cy, wd->x, wd->y, wd->w, wd->h)) continue;
        if (wd->type != W_LABEL) w->focus = i;
        switch (wd->type) {
        case W_BUTTON: wd->pressed = true; press_win = w; press_widget = wd; break;
        case W_TEXTBOX:
            wd->cursor = MIN((int)strlen(wd->text), MAX(0, (cx - wd->x - 10 + FONT_W / 2) / FONT_W));
            break;
        case W_TOGGLE:
            wd->value = !wd->value;
            if (wd->on_change) wd->on_change(w, wd);
            break;
        case W_SLIDER:
            wd->pressed = true; press_win = w; press_widget = wd;
            widget_set_from_mouse(w, wd, cx);
            break;
        case W_CHOICE: {
            int n = nsegments(wd->text), idx = (cx - wd->x) * n / MAX(1, wd->w);
            if (idx != wd->value && idx >= 0 && idx < n) { wd->value = idx; if (wd->on_change) wd->on_change(w, wd); }
            break;
        }
        case W_SWATCH: if (wd->on_click) wd->on_click(w, wd); break;
        }
        return;
    }
    if (w->on_mouse) w->on_mouse(w, cx, cy);
}

static void left_up(int x, int y) {
    drag_win = NULL;
    if (!press_widget) return;
    window_t *w = press_win;
    widget_t *wd = press_widget;
    wd->pressed = false;
    press_widget = NULL;
    press_win = NULL;
    dirty = 1;
    if (!w->used || w->closing || !wd->on_click) return;
    if (wd->type == W_SLIDER || inside(x - w->x, y - w->y - TITLE_H, wd->x, wd->y, wd->w, wd->h))
        wd->on_click(w, wd);
}

static void handle_mouse(mouse_event_t *ev) {
    int prev = mouse_btn;
    if (ev->x != mouse_x || ev->y != mouse_y) dirty = 1;
    mouse_x = ev->x; mouse_y = ev->y; mouse_btn = ev->buttons;
    if (drag_win && (mouse_btn & 1)) {
        drag_win->x = MAX(-drag_win->w + 80, MIN(mouse_x - drag_dx, gfx_width - 80));
        drag_win->y = MAX(0, MIN(mouse_y - drag_dy, gfx_height - TASKBAR_H - TITLE_H));
    }
    if (press_widget && press_widget->type == W_SLIDER && (mouse_btn & 1))
        widget_set_from_mouse(press_win, press_widget, mouse_x - press_win->x);
    if ((mouse_btn & 1) && !(prev & 1)) left_down(mouse_x, mouse_y);
    if (!(mouse_btn & 1) && (prev & 1)) left_up(mouse_x, mouse_y);
}

static void textbox_key(window_t *w, widget_t *wd, int k) {
    int len = strlen(wd->text);
    if (k == '\b') {
        if (wd->cursor > 0) { memmove(wd->text + wd->cursor - 1, wd->text + wd->cursor, len - wd->cursor + 1); wd->cursor--; }
    } else if (k == KEY_DELETE) {
        if (wd->cursor < len) memmove(wd->text + wd->cursor, wd->text + wd->cursor + 1, len - wd->cursor);
    } else if (k == KEY_LEFT) { if (wd->cursor > 0) wd->cursor--; }
    else if (k == KEY_RIGHT) { if (wd->cursor < len) wd->cursor++; }
    else if (k == KEY_HOME) wd->cursor = 0;
    else if (k == KEY_END) wd->cursor = len;
    else if (k == '\n') { if (wd->on_enter) wd->on_enter(w, wd); }
    else if (k >= 32 && k < 127 && len < (int)sizeof(wd->text) - 1) {
        memmove(wd->text + wd->cursor + 1, wd->text + wd->cursor, len - wd->cursor + 1);
        wd->text[wd->cursor++] = (char)k;
    }
}

static void handle_key(int k) {
    if (k == 27 && launcher_open) { set_launcher(false); return; }
    window_t *w = focused;
    if (!w || w->minimized || w->closing) return;
    dirty = 1;
    if (k == '\t' && w->nwidgets) {                           /* move focus */
        for (int n = 1; n <= w->nwidgets; n++) {
            int i = (w->focus + n + w->nwidgets) % w->nwidgets;
            if (w->widgets[i]->type != W_LABEL) { w->focus = i; return; }
        }
    }
    if (w->focus >= 0 && w->focus < w->nwidgets) {
        widget_t *wd = w->widgets[w->focus];
        switch (wd->type) {
        case W_TEXTBOX: textbox_key(w, wd, k); return;
        case W_BUTTON: case W_SWATCH:
            if (k == '\n' || k == ' ') { if (wd->on_click) wd->on_click(w, wd); return; }
            break;
        case W_TOGGLE:
            if (k == '\n' || k == ' ') { wd->value = !wd->value; if (wd->on_change) wd->on_change(w, wd); return; }
            break;
        case W_SLIDER: case W_CHOICE: {
            int step = wd->type == W_SLIDER ? MAX(1, (wd->max - wd->min) / 20) : 1;
            int lo = wd->type == W_SLIDER ? wd->min : 0, hi = wd->type == W_SLIDER ? wd->max : nsegments(wd->text) - 1;
            int v = wd->value + (k == KEY_RIGHT ? step : k == KEY_LEFT ? -step : 0);
            v = MAX(lo, MIN(hi, v));
            if (v != wd->value) {
                wd->value = v;
                if (wd->on_change) wd->on_change(w, wd);
                if (wd->type == W_SLIDER && wd->on_click) wd->on_click(w, wd);
            }
            if (k == KEY_LEFT || k == KEY_RIGHT) return;
            break;
        }
        }
    }
    if (w->on_key) w->on_key(w, k);
}

/* ===== per-frame updates ======================================================= */
static bool update_animations(void) {
    bool active = false;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->used) continue;
        if (w->anim) {
            if (progress(w->anim_start, duration(anim_ms[w->anim])) >= 1000) {
                if (w->anim == ANIM_CLOSE) { window_finalize(w); continue; }
                w->anim = ANIM_NONE;
            }
            active = true;
        }
        if (w->closing || w->minimized) continue;
        for (int j = 0; j < w->nwidgets; j++) {               /* toggle knobs, choice highlight */
            widget_t *wd = w->widgets[j];
            if (wd->type != W_TOGGLE && wd->type != W_CHOICE) continue;
            int target = wd->value * 1000;
            if (wd->anim == target) continue;
            int diff = target - wd->anim;
            if (!gui_settings.animations || (diff < 40 && diff > -40)) wd->anim = target;
            else wd->anim += diff * 30 / 100;
            active = true;
        }
    }
    if (progress(launcher_t, duration(LAUNCHER_MS)) < 1000) active = true;
    if (progress(fade_t, duration(FADE_MS)) < 1000) active = true;
    if (toast.active) active = true;
    return active;
}

void gui_settings_changed(bool save) {
    mouse_set_speed(gui_settings.mouse_speed);
    if (wall_rendered != gui_settings.wallpaper) render_wallpaper();
    if (save) settings_save();
    dirty = 1;
}

static bool caret_visible(void) {
    window_t *w = focused;
    if (!w || w->minimized || w->closing) return false;
    if (w->app == APP_TERMINAL) return true;
    return w->focus >= 0 && w->focus < w->nwidgets && w->widgets[w->focus]->type == W_TEXTBOX;
}

/* ===== main loop ================================================================ */
void gui_task(void *arg) {
    UNUSED(arg);
    if (gfx_available()) {
        mouse_set_bounds(gfx_width, gfx_height);
        settings_load();
        gui_settings_changed(false);
    }
    mouse_get(&mouse_x, &mouse_y, &mouse_btn);
    for (;;) {
        if (display_switch_pending) {
            display_switch_pending = 0;
            if (display_mode == DISPLAY_CLI) {
                set_launcher(false);
                console_redraw();
            } else {
                fade_t = now();
                if (!started) {
                    started = true;
                    app_terminal();
                    gui_toast("Welcome! Press F12 to return to the console.");
                }
                dirty = 1;
            }
        }
        bool anim = false;
        mouse_event_t ev;
        if (display_mode == DISPLAY_GUI && gfx_available()) {
            if (requested_app >= 0) { gui_open_app(requested_app); requested_app = -1; }
            if (apply_request) { apply_request = false; gui_settings_changed(true); }
            while (mouse_poll(&ev)) handle_mouse(&ev);
            while (kq_tail != kq_head) {
                int k = keyq[kq_tail];
                kq_tail = (kq_tail + 1) % KEYQ;
                handle_key(k);
            }
            for (int i = 0; i < MAX_WINDOWS; i++)
                if (windows[i].used && !windows[i].closing && windows[i].on_tick) windows[i].on_tick(&windows[i]);
            anim = update_animations();
            rtc_time_t t;
            rtc_read(&t);
            if ((gui_settings.seconds ? t.sec : t.min) != last_clock) dirty = 1;
            int blink = caret_visible() ? (int)gui_blink_on() : -1;
            if (blink != last_blink) { last_blink = blink; dirty = 1; }
            if (dirty || anim) { dirty = 0; compose(); }
        } else {
            while (mouse_poll(&ev)) ;                         /* discard while in CLI */
            if (apply_request) { apply_request = false; gui_settings_changed(true); }
        }
        task_sleep(anim ? 8 : 15);
    }
}
