/* Window manager and widget toolkit.
 *
 * Everything GUI-related runs in one kernel task (gui_task). Interrupt
 * handlers only queue input (mouse packets, keys); the task drains the
 * queues, updates window state and, when something changed, repaints the
 * whole desktop into the back buffer and flushes it to the screen.
 * The same task also repaints the CLI console after a mode switch. */
#include "../include/gui.h"
#include "../include/gfx.h"
#include "../include/drivers.h"
#include "../include/task.h"
#include "../include/mm.h"
#include "../include/string.h"
#include "../include/kprintf.h"

#define COL_DESKTOP_TOP 0x1E5A8C
#define COL_DESKTOP_BOT 0x0B2340
#define COL_FACE        0xC0C0C0
#define COL_TITLE_A     0x0A246A
#define COL_TITLE_B     0x3A8EE6
#define COL_TITLE_IA    0x7A7A7A
#define COL_TITLE_IB    0xB4B4B4
#define MENU_W          210
#define MENU_ITEM_H     24
#define ICON_W          80
#define ICON_H          70

static window_t  windows[MAX_WINDOWS];
static window_t *zorder[MAX_WINDOWS];      /* bottom ... top */
static int       nz;
static window_t *focused;
static volatile int dirty = 1;
static bool      started, menu_open;
static int       mouse_x, mouse_y, mouse_btn;
static window_t *drag_win;
static int       drag_dx, drag_dy;
static window_t *press_win;
static widget_t *press_widget;
static int       last_clock_sec = -1;

#define KEYQ 64
static int keyq[KEYQ];
static volatile uint32_t kq_head, kq_tail;

void gui_invalidate(void) { dirty = 1; }

void gui_post_key(int key) {               /* called from the keyboard IRQ */
    uint32_t next = (kq_head + 1) % KEYQ;
    if (next != kq_tail) { keyq[kq_head] = key; kq_head = next; }
}

/* ---- window list management ---------------------------------------------- */
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
        if (!zorder[i]->minimized) { focused = zorder[i]; break; }
}

window_t *gui_window_create(const char *title, int x, int y, int cw, int ch) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (w->used) continue;
        memset(w, 0, sizeof(*w));
        w->used = true;
        w->w = cw + 8;
        w->h = ch + TITLE_H + 5;
        w->x = MAX(0, MIN(x, gfx_width - w->w));
        w->y = MAX(0, MIN(y, gfx_height - TASKBAR_H - w->h));
        w->focus = -1;
        w->bg = COL_FACE;
        strlcpy(w->title, title, sizeof(w->title));
        zorder[nz++] = w;
        focused = w;
        dirty = 1;
        return w;
    }
    return NULL;
}

void gui_window_close(window_t *w) {
    if (!w->used) return;
    if (w->on_close) w->on_close(w);
    for (int i = 0; i < w->nwidgets; i++) kfree(w->widgets[i]);
    if (w->data && w->free_data) kfree(w->data);
    int i;
    for (i = 0; i < nz && zorder[i] != w; i++) ;
    for (; i < nz - 1; i++) zorder[i] = zorder[i + 1];
    if (nz) nz--;
    w->used = false;
    if (drag_win == w) drag_win = NULL;
    if (press_win == w) { press_win = NULL; press_widget = NULL; }
    focus_topmost();
    dirty = 1;
}

widget_t *gui_add_widget(window_t *w, int type, int x, int y, int ww, int wh, const char *text) {
    if (w->nwidgets >= MAX_WIDGETS) return NULL;
    widget_t *wd = kzalloc(sizeof(widget_t));
    if (!wd) return NULL;
    wd->type = type;
    wd->x = x; wd->y = y; wd->w = ww; wd->h = wh;
    strlcpy(wd->text, text ? text : "", sizeof(wd->text));
    wd->cursor = strlen(wd->text);
    w->widgets[w->nwidgets++] = wd;
    if (type == W_TEXTBOX && w->focus < 0) w->focus = w->nwidgets - 1;
    dirty = 1;
    return wd;
}

void gui_client_size(window_t *w, int *cw, int *ch) { *cw = w->w - 8; *ch = w->h - TITLE_H - 5; }
static int client_x(window_t *w) { return w->x + 4; }
static int client_y(window_t *w) { return w->y + TITLE_H + 1; }

/* ---- drawing ------------------------------------------------------------- */
static void text_centered(int x, int y, int w, int h, const char *s, uint32_t c) {
    int len = strlen(s);
    gfx_text(x + (w - len * FONT_W) / 2, y + (h - FONT_H) / 2, s, c);
}

static void draw_widget(window_t *win, widget_t *wd, int ox, int oy, bool has_focus) {
    int x = ox + wd->x, y = oy + wd->y;
    switch (wd->type) {
    case W_LABEL:
        gfx_text(x, y + (wd->h - FONT_H) / 2, wd->text, wd->color);
        break;
    case W_BUTTON:
        gfx_fill(x, y, wd->w, wd->h, wd->color ? wd->color : COL_FACE);
        gfx_bevel(x, y, wd->w, wd->h, !wd->pressed);
        text_centered(x + (wd->pressed ? 1 : 0), y + (wd->pressed ? 1 : 0), wd->w, wd->h, wd->text, 0);
        if (has_focus) gfx_rect(x + 4, y + 4, wd->w - 8, wd->h - 8, 0x404040);
        break;
    case W_TEXTBOX: {
        gfx_fill(x, y, wd->w, wd->h, 0xFFFFFF);
        gfx_bevel(x, y, wd->w, wd->h, false);
        int visible = (wd->w - 8) / FONT_W, len = strlen(wd->text);
        int start = wd->cursor > visible - 1 ? wd->cursor - (visible - 1) : 0;
        char buf[96];
        strlcpy(buf, wd->text + start, MIN((int)sizeof(buf), visible + 1));
        int ty = y + (wd->h - FONT_H) / 2;
        gfx_text(x + 4, ty, buf, 0);
        if (has_focus) gfx_fill(x + 4 + (wd->cursor - start) * FONT_W, ty, 2, FONT_H, 0x000080);
        UNUSED(len);
        break;
    }
    }
    UNUSED(win);
}

static void draw_window(window_t *w) {
    bool active = w == focused;
    gfx_reset_clip();
    gfx_fill(w->x + 5, w->y + 5, w->w, w->h, 0x0A1A2A);        /* drop shadow */
    gfx_fill(w->x, w->y, w->w, w->h, COL_FACE);
    gfx_bevel(w->x, w->y, w->w, w->h, true);
    gfx_set_clip(w->x + 3, w->y + 3, w->w - 6, TITLE_H - 3);
    for (int i = 0; i < w->w - 6; i++) {                        /* horizontal gradient */
        uint32_t a = active ? COL_TITLE_A : COL_TITLE_IA, b = active ? COL_TITLE_B : COL_TITLE_IB;
        int t = (i * 256) / (w->w - 6);
        uint32_t r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
        uint32_t g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
        uint32_t bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
        gfx_fill(w->x + 3 + i, w->y + 3, 1, TITLE_H - 3, RGB(r, g, bl));
    }
    gfx_text(w->x + 8, w->y + 4, w->title, 0xFFFFFF);
    gfx_reset_clip();
    int bx = w->x + w->w - 22, by = w->y + 5;                   /* close + minimize */
    gfx_fill(bx, by, 16, 14, COL_FACE); gfx_bevel(bx, by, 16, 14, true);
    gfx_char(bx + 4, by - 1, 'x', 0, 0, true);
    bx -= 18;
    gfx_fill(bx, by, 16, 14, COL_FACE); gfx_bevel(bx, by, 16, 14, true);
    gfx_char(bx + 4, by - 3, '_', 0, 0, true);

    int cx = client_x(w), cy = client_y(w), cw, ch;
    gui_client_size(w, &cw, &ch);
    gfx_set_clip(cx, cy, cw, ch);
    gfx_fill(cx, cy, cw, ch, w->bg);
    for (int i = 0; i < w->nwidgets; i++)
        draw_widget(w, w->widgets[i], cx, cy, active && w->focus == i);
    if (w->paint) w->paint(w, cx, cy);
    gfx_reset_clip();
}

/* Desktop icons: label + tiny pictogram + action. */
typedef struct { const char *label; void (*open)(void); uint32_t color; } icon_t;
static const icon_t icons[] = {
    { "Terminal",   app_terminal,     0x101010 },
    { "Files",      app_files,        0xE0B040 },
    { "Calculator", app_calculator,   0x4060C0 },
    { "Monitor",    app_sysmon,       0x30A050 },
    { "Widgets",    app_widgets_demo, 0xC05050 },
};
#define NICONS (int)(sizeof(icons) / sizeof(icons[0]))

static void draw_desktop(void) {
    gfx_gradient_v(0, 0, gfx_width, gfx_height - TASKBAR_H, COL_DESKTOP_TOP, COL_DESKTOP_BOT);
    const char *banner = "ClaudeOS";
    gfx_text(gfx_width - 8 * FONT_W - 20, gfx_height - TASKBAR_H - 44, banner, 0x9FC4E8);
    gfx_text(gfx_width - 33 * FONT_W - 20, gfx_height - TASKBAR_H - 24,
             "F12 = console  |  Start = apps", 0x6F94B8);
    for (int i = 0; i < NICONS; i++) {
        int x = 16, y = 16 + i * (ICON_H + 10);
        gfx_fill(x + 22, y + 4, 36, 32, 0xFFFFFF);
        gfx_fill(x + 24, y + 6, 32, 28, icons[i].color);
        gfx_fill(x + 24, y + 6, 32, 6, 0xD0D0D0);
        gfx_text(x + (ICON_W - (int)strlen(icons[i].label) * FONT_W) / 2 + 1, y + 43, icons[i].label, 0x000000);
        gfx_text(x + (ICON_W - (int)strlen(icons[i].label) * FONT_W) / 2, y + 42, icons[i].label, 0xFFFFFF);
    }
}

typedef struct { const char *label; void (*fn)(void); } menu_item_t;
static void menu_cli(void) { display_set_mode(DISPLAY_CLI); }
static void menu_reboot(void) { power_reboot(); }
static void menu_shutdown(void) { power_shutdown(); }
static const menu_item_t menu[] = {
    { "Terminal",            app_terminal },
    { "File Browser",        app_files },
    { "Calculator",          app_calculator },
    { "Widgets Demo",        app_widgets_demo },
    { "System Monitor",      app_sysmon },
    { "About ClaudeOS",      app_about },
    { NULL, NULL },                                  /* separator */
    { "Switch to CLI  (F12)", menu_cli },
    { "Reboot",              menu_reboot },
    { "Shut down",           menu_shutdown },
};
#define NMENU (int)(sizeof(menu) / sizeof(menu[0]))
static int menu_y(void) { return gfx_height - TASKBAR_H - NMENU * MENU_ITEM_H - 6; }

static void draw_menu(void) {
    int x = 2, y = menu_y(), h = NMENU * MENU_ITEM_H + 6;
    gfx_fill(x, y, MENU_W, h, COL_FACE);
    gfx_bevel(x, y, MENU_W, h, true);
    gfx_gradient_v(x + 3, y + 3, 22, h - 6, COL_TITLE_B, COL_TITLE_A);
    for (int i = 0; i < NMENU; i++) {
        int iy = y + 3 + i * MENU_ITEM_H;
        if (!menu[i].label) { gfx_fill(x + 30, iy + MENU_ITEM_H / 2, MENU_W - 36, 1, 0x808080); continue; }
        bool hover = mouse_x >= x && mouse_x < x + MENU_W && mouse_y >= iy && mouse_y < iy + MENU_ITEM_H;
        if (hover) gfx_fill(x + 27, iy, MENU_W - 30, MENU_ITEM_H, COL_TITLE_A);
        gfx_text(x + 34, iy + 4, menu[i].label, hover ? 0xFFFFFF : 0x000000);
    }
}

static void draw_taskbar(void) {
    int y = gfx_height - TASKBAR_H;
    gfx_fill(0, y, gfx_width, TASKBAR_H, COL_FACE);
    gfx_fill(0, y, gfx_width, 1, 0xFFFFFF);
    gfx_fill(4, y + 4, 66, TASKBAR_H - 8, COL_FACE);              /* Start button */
    gfx_bevel(4, y + 4, 66, TASKBAR_H - 8, !menu_open);
    gfx_fill(10, y + 10, 5, 5, 0xE04040); gfx_fill(16, y + 10, 5, 5, 0x40C040);
    gfx_fill(10, y + 16, 5, 5, 0x4060E0); gfx_fill(16, y + 16, 5, 5, 0xE0C040);
    gfx_text(26, y + 7, "Start", 0);
    int bx = 76;
    for (int i = 0; i < MAX_WINDOWS; i++) {                        /* one button per window */
        window_t *w = &windows[i];
        if (!w->used) continue;
        if (bx + 150 > gfx_width - 90) break;
        bool down = w == focused && !w->minimized;
        gfx_fill(bx, y + 4, 146, TASKBAR_H - 8, down ? 0xE0E0E0 : COL_FACE);
        gfx_bevel(bx, y + 4, 146, TASKBAR_H - 8, !down);
        gfx_set_clip(bx + 4, y + 4, 138, TASKBAR_H - 8);
        gfx_text(bx + 6, y + 7, w->title, 0);
        gfx_reset_clip();
        bx += 150;
    }
    rtc_time_t t;                                                  /* clock */
    rtc_read(&t);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d:%02d", t.hour, t.min, t.sec);
    last_clock_sec = t.sec;
    gfx_fill(gfx_width - 84, y + 4, 80, TASKBAR_H - 8, COL_FACE);
    gfx_bevel(gfx_width - 84, y + 4, 80, TASKBAR_H - 8, false);
    gfx_text(gfx_width - 76, y + 7, clock, 0);
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
            if (c == 'X') gfx_pixel(mouse_x + x, mouse_y + y, 0x000000);
            else if (c == 'o') gfx_pixel(mouse_x + x, mouse_y + y, 0xFFFFFF);
        }
}

static void compose(void) {
    gfx_reset_clip();
    draw_desktop();
    for (int i = 0; i < nz; i++)
        if (!zorder[i]->minimized) draw_window(zorder[i]);
    gfx_reset_clip();
    draw_taskbar();
    if (menu_open) draw_menu();
    draw_cursor();
    if (display_mode == DISPLAY_GUI) gfx_flush_all();
}

/* ---- input handling ------------------------------------------------------ */
static bool inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static window_t *window_at(int x, int y) {
    for (int i = nz - 1; i >= 0; i--) {
        window_t *w = zorder[i];
        if (!w->minimized && inside(x, y, w->x, w->y, w->w, w->h)) return w;
    }
    return NULL;
}

static void left_down(int x, int y) {
    if (menu_open) {
        menu_open = false;
        dirty = 1;
        if (inside(x, y, 2, menu_y(), MENU_W, NMENU * MENU_ITEM_H + 6)) {
            int i = (y - menu_y() - 3) / MENU_ITEM_H;
            if (i >= 0 && i < NMENU && menu[i].fn) menu[i].fn();
            return;
        }
        if (inside(x, y, 4, gfx_height - TASKBAR_H + 4, 66, TASKBAR_H - 8)) return;
    }
    if (y >= gfx_height - TASKBAR_H) {                       /* taskbar */
        if (inside(x, y, 4, gfx_height - TASKBAR_H + 4, 66, TASKBAR_H - 8)) { menu_open = true; dirty = 1; return; }
        int bx = 76;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t *w = &windows[i];
            if (!w->used) continue;
            if (x >= bx && x < bx + 146) {
                if (w == focused && !w->minimized) { w->minimized = true; focus_topmost(); }
                else { w->minimized = false; raise_window(w); }
                dirty = 1;
                return;
            }
            bx += 150;
        }
        return;
    }
    window_t *w = window_at(x, y);
    if (!w) {                                               /* desktop: icons */
        focused = NULL;
        dirty = 1;
        for (int i = 0; i < NICONS; i++)
            if (inside(x, y, 16, 16 + i * (ICON_H + 10), ICON_W, ICON_H)) { icons[i].open(); return; }
        return;
    }
    raise_window(w);
    if (y < w->y + TITLE_H) {                               /* title bar */
        int bx = w->x + w->w - 22, by = w->y + 5;
        if (inside(x, y, bx, by, 16, 14)) { gui_window_close(w); return; }
        if (inside(x, y, bx - 18, by, 16, 14)) { w->minimized = true; focus_topmost(); return; }
        drag_win = w;
        drag_dx = x - w->x;
        drag_dy = y - w->y;
        return;
    }
    int cx = x - client_x(w), cy = y - client_y(w);
    for (int i = 0; i < w->nwidgets; i++) {
        widget_t *wd = w->widgets[i];
        if (!inside(cx, cy, wd->x, wd->y, wd->w, wd->h)) continue;
        if (wd->type == W_BUTTON) { wd->pressed = true; press_win = w; press_widget = wd; w->focus = i; }
        if (wd->type == W_TEXTBOX) {
            w->focus = i;
            wd->cursor = MIN((int)strlen(wd->text), MAX(0, (cx - wd->x - 4 + FONT_W / 2) / FONT_W));
        }
        return;
    }
    if (w->on_mouse) w->on_mouse(w, cx, cy);
}

static void left_up(int x, int y) {
    drag_win = NULL;
    if (press_widget) {
        window_t *w = press_win;
        widget_t *wd = press_widget;
        wd->pressed = false;
        press_widget = NULL;
        press_win = NULL;
        dirty = 1;
        if (w->used && inside(x - client_x(w), y - client_y(w), wd->x, wd->y, wd->w, wd->h) && wd->on_click)
            wd->on_click(w, wd);
    }
}

static void handle_mouse(mouse_event_t *ev) {
    int prev = mouse_btn;
    if (ev->x != mouse_x || ev->y != mouse_y) dirty = 1;
    mouse_x = ev->x; mouse_y = ev->y; mouse_btn = ev->buttons;
    if (drag_win && (mouse_btn & 1)) {
        drag_win->x = MAX(-drag_win->w + 60, MIN(mouse_x - drag_dx, gfx_width - 60));
        drag_win->y = MAX(0, MIN(mouse_y - drag_dy, gfx_height - TASKBAR_H - TITLE_H));
    }
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
    window_t *w = focused;
    if (!w || w->minimized) return;
    dirty = 1;
    if (k == '\t' && w->nwidgets) {                           /* move focus */
        for (int n = 1; n <= w->nwidgets; n++) {
            int i = (w->focus + n + w->nwidgets) % w->nwidgets;
            if (w->widgets[i]->type != W_LABEL) { w->focus = i; return; }
        }
    }
    if (w->focus >= 0 && w->focus < w->nwidgets) {
        widget_t *wd = w->widgets[w->focus];
        if (wd->type == W_TEXTBOX) { textbox_key(w, wd, k); return; }
        if (wd->type == W_BUTTON && (k == '\n' || k == ' ')) {
            if (wd->on_click) wd->on_click(w, wd);
            return;
        }
    }
    if (w->on_key) w->on_key(w, k);
}

/* ---- main loop ------------------------------------------------------------ */
void gui_task(void *arg) {
    UNUSED(arg);
    if (gfx_available()) mouse_set_bounds(gfx_width, gfx_height);
    mouse_get(&mouse_x, &mouse_y, &mouse_btn);
    for (;;) {
        if (display_switch_pending) {
            display_switch_pending = 0;
            if (display_mode == DISPLAY_CLI) console_redraw();
            else {
                if (!started) { started = true; app_terminal(); }
                dirty = 1;
            }
        }
        mouse_event_t ev;
        if (display_mode == DISPLAY_GUI && gfx_available()) {
            while (mouse_poll(&ev)) handle_mouse(&ev);
            while (kq_tail != kq_head) {
                int k = keyq[kq_tail];
                kq_tail = (kq_tail + 1) % KEYQ;
                handle_key(k);
            }
            for (int i = 0; i < MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].on_tick) windows[i].on_tick(&windows[i]);
            rtc_time_t t;
            rtc_read(&t);
            if (t.sec != last_clock_sec) dirty = 1;
            if (dirty) { dirty = 0; compose(); }
        } else {
            while (mouse_poll(&ev)) ;                         /* discard while in CLI */
        }
        task_sleep(15);
    }
}
