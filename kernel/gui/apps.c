/* Built-in GUI applications. All functions here run in the GUI task. */
#include "../include/gui.h"
#include "../include/gfx.h"
#include "../include/task.h"
#include "../include/shell.h"
#include "../include/fs.h"
#include "../include/mm.h"
#include "../include/io.h"
#include "../include/drivers.h"
#include "../include/string.h"
#include "../include/kprintf.h"
#include "../include/clipboard.h"

static bool inside_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ===== Terminal: a termbuf + tty + its own shell task ====================== */
#define TERM_COLS 80
#define TERM_ROWS 25
#define TERM_PX   12
#define TERM_PY   10

/* Window-side state: the shared tty plus a mouse text selection (in cells). */
typedef struct {
    tty_t *tty;
    bool   sel;
    int    ax, ay, bx, by;          /* anchor and end cell of the selection */
} term_t;

static void term_hook_cell(termbuf_t *t, int x, int y) { UNUSED(t); UNUSED(x); UNUSED(y); gui_invalidate(); }
static void term_hook_any(termbuf_t *t) { UNUSED(t); gui_invalidate(); }
static void term_hook_cursor(termbuf_t *t, int x, int y) { UNUSED(t); UNUSED(x); UNUSED(y); gui_invalidate(); }

/* The tty is shared by the window and the shell task; the last one frees it. */
static void term_release(tty_t *tty) {
    uint32_t f = irq_save();
    if (--tty->refs == 0) {
        kfree(tty->tb->cells);
        kfree(tty->tb);
        kfree(tty);
    }
    irq_restore(f);
}

void gui_terminal_release(tty_t *tty) {      /* called by the exiting shell */
    tty->hangup = 1;
    term_release(tty);
}

/* Selection as linear cell indexes [a, b). */
static bool term_sel_range(term_t *t, int *a, int *b) {
    if (!t->sel) return false;
    int p = t->ay * TERM_COLS + t->ax, q = t->by * TERM_COLS + t->bx;
    *a = MIN(p, q);
    *b = MAX(p, q) + 1;
    return true;
}

static void term_paint(window_t *w, int cx, int cy) {
    term_t *t = w->data;
    gfx_fill(cx, cy, w->w, w->h - TITLE_H, vga_palette[0]);
    if (!t || !t->tty) return;
    termbuf_t *tb = t->tty->tb;
    int sa = 0, sb = 0;
    if (term_sel_range(t, &sa, &sb))
        for (int i = sa; i < sb; i++)
            gfx_fill(cx + TERM_PX + (i % TERM_COLS) * FONT_W, cy + TERM_PY + (i / TERM_COLS) * FONT_H,
                     FONT_W, FONT_H, gfx_mix(vga_palette[0], gui_accent(), 110));
    for (int y = 0; y < tb->rows; y++)
        for (int x = 0; x < tb->cols; x++) {
            uint16_t v = tb->cells[y * tb->cols + x];
            uint8_t ch = v & 0xFF, attr = v >> 8;
            int px = cx + TERM_PX + x * FONT_W, py = cy + TERM_PY + y * FONT_H;
            if (attr >> 4) gfx_fill(px, py, FONT_W, FONT_H, vga_palette[attr >> 4]);
            if (ch != ' ') gfx_char(px, py, (char)ch, vga_palette[attr & 15], 0, true);
        }
    if (t->tty->hangup) return;
    int px = cx + TERM_PX + tb->cx * FONT_W, py = cy + TERM_PY + tb->cy * FONT_H;
    if (!gui_is_focused(w)) gfx_rect(px, py, FONT_W, FONT_H, vga_palette[8]);
    else if (gui_blink_on()) gfx_fill(px, py + FONT_H - 3, FONT_W, 3, gui_accent());
}

static void term_copy(window_t *w, widget_t *wd) {
    UNUSED(wd);
    term_t *t = w->data;
    int a, b, n = 0;
    if (!t || !term_sel_range(t, &a, &b)) return;
    char *buf = kmalloc(TERM_COLS * TERM_ROWS + TERM_ROWS + 1);
    if (!buf) return;
    termbuf_t *tb = t->tty->tb;
    for (int i = a; i < b; i++) {
        buf[n++] = (char)(tb->cells[i] & 0xFF);
        if ((i + 1) % TERM_COLS == 0 || i == b - 1) {        /* end of a row: trim trailing blanks */
            while (n && buf[n - 1] == ' ') n--;
            if (i != b - 1) buf[n++] = '\n';
        }
    }
    clipboard_set(buf, n);
    kfree(buf);
    t->sel = false;
    gui_toast("Copied to clipboard");
}

static void term_paste(window_t *w, widget_t *wd) {
    UNUSED(wd);
    term_t *t = w->data;
    if (!t || !t->tty) return;
    if (!clipboard_len()) { gui_toast("Clipboard is empty"); return; }
    tty_paste(t->tty);
}

static void term_select_all(window_t *w, widget_t *wd) {
    UNUSED(wd);
    term_t *t = w->data;
    t->sel = true; t->ax = 0; t->ay = 0; t->bx = TERM_COLS - 1; t->by = TERM_ROWS - 1;
}

static void term_key(window_t *w, int k) {
    term_t *t = w->data;
    if (!t || !t->tty) return;
    if (k == KEY_CTRL_C && t->sel) { term_copy(w, NULL); return; }   /* copy, else interrupt */
    if (k == KEY_CTRL_V) { term_paste(w, NULL); return; }
    t->sel = false;
    tty_input(t->tty, k & ~KEY_SHIFT);
}

static void term_mouse(window_t *w, int x, int y, int ev) {
    term_t *t = w->data;
    int col = MAX(0, MIN(TERM_COLS - 1, (x - TERM_PX) / FONT_W));
    int row = MAX(0, MIN(TERM_ROWS - 1, (y - TERM_PY) / FONT_H));
    if (ev == MOUSE_DOWN) { t->ax = t->bx = col; t->ay = t->by = row; t->sel = false; }
    else if (ev == MOUSE_DRAG) { t->bx = col; t->by = row; t->sel = t->bx != t->ax || t->by != t->ay; }
    gui_invalidate();
}

static void term_context(window_t *w, int x, int y) {
    UNUSED(x); UNUSED(y);
    term_t *t = w->data;
    gui_menu_item_t items[] = {
        { "Copy", term_copy, !t->sel },
        { "Paste", term_paste, clipboard_len() == 0 },
        { NULL, NULL, false },
        { "Select all", term_select_all, false },
    };
    gui_show_menu(items, 4, w, NULL);
}

static void term_close(window_t *w) {
    term_t *t = w->data;
    if (!t || !t->tty) return;
    tty_t *tty = t->tty;
    tty->hangup = 1;                          /* the shell sees this and exits */
    if (tty->fg_pid) task_kill(tty->fg_pid);
    t->tty = NULL;
    term_release(tty);
}

static void term_tick(window_t *w) {
    term_t *t = w->data;
    if (t && t->tty && t->tty->hangup) gui_window_close(w);   /* shell exited ('exit') */
}

void app_terminal(void) {
    tty_t *tty = kzalloc(sizeof(tty_t));
    termbuf_t *tb = kzalloc(sizeof(termbuf_t));
    uint16_t *cells = kmalloc(TERM_COLS * TERM_ROWS * 2);
    term_t *t = kzalloc(sizeof(term_t));
    if (!tty || !tb || !cells || !t) { kfree(tty); kfree(tb); kfree(cells); kfree(t); return; }
    tb->on_cell = term_hook_cell;
    tb->on_redraw = term_hook_any;
    tb->on_scroll = term_hook_any;
    tb->on_cursor = term_hook_cursor;
    termbuf_init(tb, TERM_COLS, TERM_ROWS, cells);
    tty_init(tty, tb);
    tty->refs = 2;
    t->tty = tty;
    window_t *w = gui_window_create(APP_TERMINAL, "Terminal", -1, -1,
                                    TERM_COLS * FONT_W + 2 * TERM_PX, TERM_ROWS * FONT_H + 2 * TERM_PY);
    if (!w) { kfree(tty); kfree(tb); kfree(cells); kfree(t); return; }
    w->data = t;
    w->free_data = true;
    w->paint = term_paint;
    w->on_key = term_key;
    w->on_mouse = term_mouse;
    w->on_context = term_context;
    w->on_close = term_close;
    w->on_tick = term_tick;
    int pid = task_create_kernel("shell", shell_main, tty, tty);
    if (pid < 0) { tty->refs = 1; gui_window_close(w); return; }
    strlcpy(w->title, "Terminal (pid %d)", sizeof(w->title));
    w->title_num = pid;
}

/* ===== Calculator ========================================================== */
typedef struct { int acc, cur; char op; bool fresh, error; } calc_t;

static int calc_apply(calc_t *c, int a, char op, int b) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (b == 0) { c->error = true; return 0; }
        if (b == -1) return -a;              /* avoids INT_MIN / -1 trap */
        return a / b;
    }
    return b;
}

static void calc_press(calc_t *c, char k) {
    if (k == 'C') { memset(c, 0, sizeof(*c)); return; }
    if (c->error) return;
    if (k >= '0' && k <= '9') {
        if (c->fresh) { c->cur = 0; c->fresh = false; }
        if (c->cur < 100000000 && c->cur > -100000000) c->cur = c->cur * 10 + (c->cur < 0 ? -(k - '0') : k - '0');
    } else if (k == '<') {
        if (!c->fresh) c->cur /= 10;
    } else if (k == 'N') {
        c->cur = -c->cur;
    } else if (k == '%') {
        c->cur = c->op ? c->acc / 100 * c->cur + c->acc % 100 * c->cur / 100 : c->cur / 100;
    } else if (k == '=') {
        if (c->op) { c->acc = calc_apply(c, c->acc, c->op, c->cur); c->op = 0; }
        c->cur = c->acc;
        c->fresh = true;
    } else if (strchr("+-*/", k)) {
        if (!c->fresh || !c->op) c->acc = c->op ? calc_apply(c, c->acc, c->op, c->cur) : c->cur;
        c->op = k;
        c->fresh = true;
        c->cur = c->acc;
    }
}

static void calc_paint(window_t *w, int cx, int cy) {
    calc_t *c = w->data;
    const theme_t *th = gui_theme();
    int x = cx + 12, y = cy + 12, ww = w->w - 24;
    gfx_round_fill(x, y, ww, 84, 12, th->card);
    char buf[24];
    if (c->op) {
        snprintf(buf, sizeof(buf), "%d %c", c->acc, c->op);
        gfx_text(x + ww - 14 - (int)strlen(buf) * FONT_W, y + 10, buf, th->dim);
    }
    if (c->error) strlcpy(buf, "Error", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%d", c->cur);
    int len = strlen(buf), scale = len > 12 ? 1 : 2;
    gfx_text_scaled(x + ww - 14 - len * FONT_W * scale, y + 74 - FONT_H * scale, buf,
                    c->error ? 0xEF5350 : th->text, scale);
}

static void calc_click(window_t *w, widget_t *wd) { calc_press(w->data, (char)wd->tag); }

static void calc_key(window_t *w, int k) {
    calc_t *c = w->data;
    if (k == KEY_CTRL_C) {                               /* copy the displayed number */
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", c->cur);
        clipboard_set(buf, strlen(buf));
        gui_toast("Copied to clipboard");
        return;
    }
    if (k == KEY_CTRL_V) {                               /* paste digits as key presses */
        char buf[32];
        int n = clipboard_get(buf, sizeof(buf));
        bool neg = n && buf[0] == '-';
        for (int i = 0; i < n; i++) if (buf[i] >= '0' && buf[i] <= '9') calc_press(c, buf[i]);
        if (neg) calc_press(c, 'N');
        return;
    }
    k &= ~KEY_SHIFT;
    if (k == '\n') k = '=';
    if (k == '\b') k = '<';
    if (k == 'c' || k == 27) k = 'C';
    if ((k >= '0' && k <= '9') || (k && k < 128 && strchr("+-*/=C<%", k))) calc_press(w->data, (char)k);
}

void app_calculator(void) {
    window_t *w = gui_window_create(APP_CALC, "Calculator", -1, -1, 260, 392);
    if (!w) return;
    w->data = kzalloc(sizeof(calc_t));
    w->free_data = true;
    w->paint = calc_paint;
    w->on_key = calc_key;
    static const struct { const char *label; char key; int style; } keys[20] = {
        { "C", 'C', S_DANGER }, { "+/-", 'N', S_NORMAL }, { "%", '%', S_NORMAL }, { "/", '/', S_ACCENT },
        { "7", '7', 0 }, { "8", '8', 0 }, { "9", '9', 0 }, { "*", '*', S_ACCENT },
        { "4", '4', 0 }, { "5", '5', 0 }, { "6", '6', 0 }, { "-", '-', S_ACCENT },
        { "1", '1', 0 }, { "2", '2', 0 }, { "3", '3', 0 }, { "+", '+', S_ACCENT },
        { "0", '0', 0 }, { NULL, 0, 0 }, { "<", '<', S_NORMAL }, { "=", '=', S_PRIMARY },
    };
    for (int i = 0; i < 20; i++) {
        if (!keys[i].label) continue;
        int col = i % 4, row = i / 4, bw = keys[i].key == '0' ? 114 : 54;
        widget_t *b = gui_add_widget(w, W_BUTTON, 12 + col * 60, 108 + row * 56, bw, 48, keys[i].label);
        b->on_click = calc_click;
        b->tag = keys[i].key;
        b->style = keys[i].style;
    }
}

/* ===== Widgets demo ========================================================== */
typedef struct { widget_t *name, *result, *count, *volume, *note, *status; int clicks; } demo_t;

static void demo_greet(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    if (d->name->text[0]) snprintf(d->result->text, sizeof(d->result->text), T("Hello, %s! Welcome to ClaudeOS."), d->name->text);
    else strlcpy(d->result->text, T("Please type your name first."), sizeof(d->result->text));
}

static void demo_click(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    d->clicks++;
    snprintf(d->count->text, sizeof(d->count->text), T("Clicked: %d"), d->clicks);
}

static void demo_notify(window_t *w, widget_t *wd) {
    UNUSED(w);
    gui_toast(wd->value ? "Notifications enabled" : "Notifications disabled");
}

static void demo_volume(window_t *w, widget_t *wd) {
    demo_t *d = w->data;
    snprintf(d->volume->text, sizeof(d->volume->text), "%d%%", wd->value);
}

static void demo_size(window_t *w, widget_t *wd) {
    UNUSED(w);
    static const char *names[3] = { "Small", "Medium", "Large" };
    char buf[48];
    snprintf(buf, sizeof(buf), T("Size: %s"), T(names[wd->value % 3]));
    gui_toast(buf);
}

static void demo_save(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    char buf[96];
    int r = fs_write("/note.txt", d->note->text, strlen(d->note->text), false);
    if (r < 0) snprintf(buf, sizeof(buf), T("Save failed: %s"), fs_strerror(r));
    else snprintf(buf, sizeof(buf), T("Saved %d bytes to %s"), r, "/note.txt");
    gui_toast(buf);
}

static void demo_load(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    char buf[96];
    int r = fs_read("/note.txt", 0, d->note->text, sizeof(d->note->text) - 1);
    if (r < 0) { snprintf(buf, sizeof(buf), T("Load failed: %s"), fs_strerror(r)); gui_toast(buf); return; }
    d->note->text[r] = 0;
    for (int i = 0; i < r; i++) if (d->note->text[i] == '\n') d->note->text[i] = ' ';
    d->note->cursor = r;
    snprintf(buf, sizeof(buf), T("Loaded %d bytes from %s"), r, "/note.txt");
    gui_toast(buf);
}

void app_widgets_demo(void) {
    window_t *w = gui_window_create(APP_WIDGETS, "Widgets Demo", -1, -1, 470, 412);
    if (!w) return;
    demo_t *d = kzalloc(sizeof(demo_t));
    w->data = d;
    w->free_data = true;
    gui_add_widget(w, W_LABEL, 16, 12, 200, 20, "Text input")->style = S_HEADING;
    d->name = gui_add_widget(w, W_TEXTBOX, 16, 38, 300, 34, "");
    strlcpy(d->name->placeholder, "Type your name...", sizeof(d->name->placeholder));
    d->name->on_enter = demo_greet;
    widget_t *b = gui_add_widget(w, W_BUTTON, 326, 38, 128, 34, "Greet");
    b->style = S_PRIMARY;
    b->on_click = demo_greet;
    d->result = gui_add_widget(w, W_LABEL, 16, 80, 440, 20, "Type a name and press Enter.");
    d->result->style = S_ACCENT;

    gui_add_widget(w, W_LABEL, 16, 114, 200, 20, "Controls")->style = S_HEADING;
    gui_add_widget(w, W_BUTTON, 16, 140, 120, 34, "Click me")->on_click = demo_click;
    d->count = gui_add_widget(w, W_LABEL, 150, 140, 200, 34, "Clicked: 0");
    d->count->style = S_DIM;
    gui_add_widget(w, W_LABEL, 16, 186, 200, 26, "Notifications");
    widget_t *t = gui_add_widget(w, W_TOGGLE, 410, 187, 44, 24, "");
    t->value = 1; t->anim = 1000;
    t->on_change = demo_notify;
    gui_add_widget(w, W_LABEL, 16, 224, 100, 26, "Volume");
    widget_t *s = gui_add_widget(w, W_SLIDER, 120, 224, 280, 26, "");
    s->value = 60;
    s->on_change = demo_volume;
    d->volume = gui_add_widget(w, W_LABEL, 412, 224, 50, 26, "60%");
    d->volume->style = S_DIM;
    gui_add_widget(w, W_LABEL, 16, 262, 100, 32, "Size");
    widget_t *c = gui_add_widget(w, W_CHOICE, 120, 262, 334, 32, "Small|Medium|Large");
    c->value = 1; c->anim = 1000;
    c->on_change = demo_size;

    gui_add_widget(w, W_LABEL, 16, 310, 200, 20, "Files")->style = S_HEADING;
    d->note = gui_add_widget(w, W_TEXTBOX, 16, 336, 262, 34, "");
    strlcpy(d->note->placeholder, "Write a note...", sizeof(d->note->placeholder));
    b = gui_add_widget(w, W_BUTTON, 288, 336, 80, 34, "Save");
    b->style = S_PRIMARY;
    b->on_click = demo_save;
    gui_add_widget(w, W_BUTTON, 376, 336, 78, 34, "Load")->on_click = demo_load;
    d->status = gui_add_widget(w, W_LABEL, 16, 380, 440, 20, "Tab moves focus, Space/arrows change controls.");
    d->status->style = S_DIM;
}

/* ===== System monitor ======================================================= */
#define CPU_SAMPLES 60
typedef struct { uint32_t last_tick, last_idle; int hist[CPU_SAMPLES]; int n; } sysmon_t;

static void card(int x, int y, int w, int h, const char *title) {
    const theme_t *th = gui_theme();
    gfx_round_fill(x, y, w, h, 12, th->card);
    if (title) gfx_text(x + 14, y + 10, title, th->dim);
}

static void bar(int x, int y, int w, uint32_t used, uint32_t total) {
    uint32_t u = used >> 8, t = total >> 8;                 /* no 64-bit division */
    int fill = t ? (int)(u * (uint32_t)w / t) : 0;
    gfx_round_fill(x, y, w, 8, 4, gui_theme()->button_hover);
    if (fill > 0) gfx_round_fill(x, y, MAX(fill, 8), 8, 4, gui_accent());
}

static void sysmon_paint(window_t *w, int cx, int cy) {
    sysmon_t *m = w->data;
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    char buf[96];
    int x = cx + 16, W = w->w - 32;

    /* summary cards */
    int cw = (W - 24) / 3, cpu = m->n ? m->hist[m->n - 1] : 0;
    uint32_t s = timer_ticks() / 1000;
    uint32_t tot = pmm_total_frames() * 4, fr = pmm_free_frames() * 4;
    int ntasks = 0;
    for (int i = 0; i < MAX_TASKS; i++) if (task_by_index(i)->state != T_UNUSED) ntasks++;
    const char *titles[3] = { "Uptime", "CPU", "Tasks" };
    char vals[3][16];
    snprintf(vals[0], 16, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    snprintf(vals[1], 16, "%d%%", cpu);
    snprintf(vals[2], 16, "%d", ntasks);
    for (int i = 0; i < 3; i++) {
        card(x + i * (cw + 12), cy + 14, cw, 74, T(titles[i]));
        gfx_text_scaled(x + i * (cw + 12) + 14, cy + 44, vals[i], i == 1 ? acc : th->text, 2);
    }

    /* CPU history graph */
    int gy = cy + 102;
    card(x, gy, W, 140, T("CPU usage, last 30 s"));
    int ax = x + 14, ay = gy + 34, aw = W - 28, ah = 92;
    for (int i = 0; i <= 4; i++) gfx_fill(ax, ay + ah * i / 4, aw, 1, gfx_mix(th->card, th->dim, 50));
    for (int col = 0; col < aw && m->n > 1; col++) {
        int pos = col * (CPU_SAMPLES - 1) * 16 / aw;              /* 1/16 sample precision */
        int i0 = pos / 16, frac = pos % 16;
        int start = CPU_SAMPLES - m->n;                           /* newest samples on the right */
        if (i0 < start) continue;
        int v0 = m->hist[i0 - start], v1 = i0 + 1 - start < m->n ? m->hist[i0 + 1 - start] : v0;
        int v = (v0 * (16 - frac) + v1 * frac) / 16, hgt = v * ah / 100;
        gfx_blend(ax + col, ay + ah - hgt, 1, hgt, acc, 70);
        gfx_fill(ax + col, ay + ah - hgt - 1, 1, 2, acc);
    }

    /* memory */
    int my = gy + 154;
    card(x, my, W, 74, NULL);
    uint32_t ht, hu, hb;
    heap_stats(&ht, &hu, &hb);
    int half = (W - 42) / 2;
    snprintf(buf, sizeof(buf), T("RAM  %u / %u MB"), (tot - fr) / 1024, tot / 1024);
    gfx_text(x + 14, my + 14, buf, th->text);
    bar(x + 14, my + 44, half, tot - fr, tot);
    snprintf(buf, sizeof(buf), T("Heap  %u / %u KB"), hu / 1024, ht / 1024);
    gfx_text(x + 28 + half, my + 14, buf, th->text);
    bar(x + 28 + half, my + 44, half, hu, ht);

    /* task list */
    int ty = my + 88;
    card(x, ty, W, w->h - TITLE_H - (ty - cy) - 14, NULL);
    gfx_text(x + 14, ty + 10, T("PID  STATE      CPU(ms)  TYPE    NAME"), th->dim);
    int row = 0;
    for (int i = 0; i < MAX_TASKS && row < 8; i++) {
        task_t *t = task_by_index(i);
        if (t->state == T_UNUSED) continue;
        int ry = ty + 32 + row * 20;
        if (row % 2 == 0) gfx_round_blend(x + 6, ry - 2, W - 12, 20, 6, th->hover, 120);
        snprintf(buf, sizeof(buf), "%3d  %-9s %8u  %-6s  %s", t->pid, task_state_name(t->state),
                 t->cpu_ticks, t->is_user ? "user" : "kernel", t->name);
        gfx_text(x + 14, ry, buf, t->is_user ? acc : th->text);
        row++;
    }
}

static void sysmon_tick(window_t *w) {
    sysmon_t *m = w->data;
    uint32_t nowt = timer_ticks(), idle = task_by_index(0)->cpu_ticks;
    if (nowt - m->last_tick < 500) return;
    if (m->last_tick) {
        uint32_t el = nowt - m->last_tick, di = idle - m->last_idle;
        int usage = 100 - (int)(di * 100 / (el ? el : 1));
        usage = MAX(0, MIN(100, usage));
        if (m->n == CPU_SAMPLES) { memmove(m->hist, m->hist + 1, (CPU_SAMPLES - 1) * sizeof(int)); m->n--; }
        m->hist[m->n++] = usage;
    }
    m->last_tick = nowt;
    m->last_idle = idle;
    gui_invalidate();
}

void app_sysmon(void) {
    window_t *w = gui_window_create(APP_MONITOR, "System Monitor", -1, -1, 500, 528);
    if (!w) return;
    w->data = kzalloc(sizeof(sysmon_t));
    w->free_data = true;
    w->paint = sysmon_paint;
    w->on_tick = sysmon_tick;
}

/* ===== About ================================================================ */
static void about_ok(window_t *w, widget_t *wd) { UNUSED(wd); gui_window_close(w); }

static void about_paint(window_t *w, int cx, int cy) {
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    int mid = cx + w->w / 2;
    gfx_round_gradient(mid - 40, cy + 22, 80, 80, 22, gfx_mix(acc, 0xFFFFFF, 70), acc);
    gui_draw_logo(mid - 3, cy + 65, 22, 0xFFFFFF);
    gfx_text_scaled(mid - 64, cy + 116, "ClaudeOS", th->text, 2);
    static const char *lines[] = {
        "Version 1.2 for i386",
        "",
        "C + NASM kernel, preemptive multitasking,",
        "FAT12/16, ring-3 programs and this desktop.",
        "Font: Terminus (SIL Open Font License)",
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        gfx_text(mid - gfx_text_width(T(lines[i])) / 2, cy + 156 + i * 18, T(lines[i]), i == 0 ? acc : th->dim);
}

void app_about(void) {
    window_t *w = gui_window_create(APP_ABOUT, "About ClaudeOS", gfx_width / 2 - 220, gfx_height / 2 - 200, 440, 300);
    if (!w) return;
    w->paint = about_paint;
    widget_t *b = gui_add_widget(w, W_BUTTON, 170, 254, 100, 34, "OK");
    b->style = S_PRIMARY;
    b->on_click = about_ok;
}

/* ===== File browser ========================================================= */
#define FB_MAX  64
#define FB_ROW  24
#define FB_ROWS 13
typedef struct {
    char cwd[128];
    fs_entry_t ent[FB_MAX];
    int n, sel, top;
    char preview[1600];
    char status[96];
    widget_t *path;
} files_t;

static char file_clip[128];                   /* path copied with Ctrl+C in Files */

static void files_path(files_t *f, int i, char *out) { path_normalize(f->cwd, f->ent[i].name, out, 128); }

static void files_load(files_t *f) {
    f->n = fs_list(f->cwd, f->ent, FB_MAX);
    if (f->n < 0) { snprintf(f->status, sizeof(f->status), "%s", fs_strerror(f->n)); f->n = 0; }
    else snprintf(f->status, sizeof(f->status), T("%d item(s)  -  right-click for actions"), f->n);
    f->sel = -1;
    f->top = 0;
    f->preview[0] = 0;
    strlcpy(f->path->text, f->cwd, sizeof(f->path->text));
}

static void files_select(files_t *f, int i) {
    if (i < 0 || i >= f->n) return;
    f->sel = i;
    if (i < f->top) f->top = i;
    if (i >= f->top + FB_ROWS) f->top = i - FB_ROWS + 1;
    char p[128];
    files_path(f, i, p);
    if (f->ent[i].is_dir) {
        snprintf(f->preview, sizeof(f->preview), T("[folder]\n\nPress Enter or click it again to open."));
        snprintf(f->status, sizeof(f->status), "%s/", p);
        return;
    }
    int r = fs_read(p, 0, f->preview, sizeof(f->preview) - 1);
    f->preview[r > 0 ? r : 0] = 0;
    bool binary = false;
    for (int k = 0; k < r; k++) if (!f->preview[k] || (uint8_t)f->preview[k] > 127) binary = true;
    if (binary) snprintf(f->preview, sizeof(f->preview), T("[binary file, %u bytes]\n\nRun it from a terminal:\n  %s"), f->ent[i].size, p);
    snprintf(f->status, sizeof(f->status), T("%s - %u bytes"), p, f->ent[i].size);
}

static void files_open_sel(window_t *w, widget_t *wd) {
    UNUSED(wd);
    files_t *f = w->data;
    if (f->sel < 0) return;
    char p[128];
    files_path(f, f->sel, p);
    if (f->ent[f->sel].is_dir) { strlcpy(f->cwd, p, sizeof(f->cwd)); files_load(f); }
    else app_notes_open(p);
}

static void files_up(window_t *w, widget_t *wd) {
    UNUSED(wd);
    files_t *f = w->data;
    char p[128];
    path_normalize(f->cwd, "..", p, sizeof(p));
    strlcpy(f->cwd, p, sizeof(f->cwd));
    files_load(f);
}

static void files_refresh(window_t *w, widget_t *wd) { UNUSED(wd); files_load(w->data); }

static void files_copy(window_t *w, widget_t *wd) {
    UNUSED(wd);
    files_t *f = w->data;
    if (f->sel < 0 || f->ent[f->sel].is_dir) { gui_toast("Select a file to copy"); return; }
    files_path(f, f->sel, file_clip);
    clipboard_set(file_clip, strlen(file_clip));              /* the path is also text */
    gui_toast("File copied - paste it with Ctrl+V");
}

/* Paste the copied file into the current folder, picking a free 8.3 name. */
static void files_paste(window_t *w, widget_t *wd) {
    UNUSED(wd);
    files_t *f = w->data;
    fs_entry_t st;
    if (!file_clip[0] || fs_stat(file_clip, &st) < 0 || st.is_dir) { gui_toast("Nothing to paste"); return; }
    const char *name = strrchr(file_clip, '/') + 1;
    char base[16], ext[8] = "", dst[128];
    strlcpy(base, name, sizeof(base));
    char *dot = strchr(base, '.');
    if (dot) { strlcpy(ext, dot, sizeof(ext)); *dot = 0; }
    path_normalize(f->cwd, name, dst, sizeof(dst));
    for (int i = 1; fs_stat(dst, &st) == 0 && i < 10; i++) {    /* name exists: NAME_1.EXT ... */
        char alt[16];
        base[6] = 0;
        snprintf(alt, sizeof(alt), "%s_%d%s", base, i, ext);
        path_normalize(f->cwd, alt, dst, sizeof(dst));
    }
    uint8_t *buf = kmalloc(st.size + 1);
    if (!buf) return;
    int r = fs_read(file_clip, 0, buf, st.size);
    if (r >= 0) r = fs_write(dst, buf, st.size, false);
    kfree(buf);
    char msg[96];
    if (r < 0) snprintf(msg, sizeof(msg), "%s", fs_strerror(r));
    else snprintf(msg, sizeof(msg), T("Pasted as %s"), dst);
    gui_toast(msg);
    files_load(f);
}

static void files_delete(window_t *w, widget_t *wd) {
    UNUSED(wd);
    files_t *f = w->data;
    if (f->sel < 0) return;
    char p[128], msg[96];
    files_path(f, f->sel, p);
    int r = fs_remove(p);
    if (r < 0) snprintf(msg, sizeof(msg), "%s: %s", f->ent[f->sel].name, fs_strerror(r));
    else snprintf(msg, sizeof(msg), T("Deleted %s"), f->ent[f->sel].name);
    gui_toast(msg);
    int keep = f->sel;
    files_load(f);
    if (f->n) files_select(f, MIN(keep, f->n - 1));
}

static void files_new_folder(window_t *w, widget_t *wd) {
    UNUSED(wd);
    files_t *f = w->data;
    char name[16], p[128];
    fs_entry_t st;
    for (int i = 0; i < 10; i++) {
        if (i) snprintf(name, sizeof(name), "folder%d", i); else strlcpy(name, "folder", sizeof(name));
        path_normalize(f->cwd, name, p, sizeof(p));
        if (fs_stat(p, &st) < 0) break;
    }
    int r = fs_mkdir(p);
    gui_toast(r < 0 ? fs_strerror(r) : T("Folder created"));
    files_load(f);
}

static int files_row_at(files_t *f, int x, int y) {
    if (x < 22 || x > 280 || y < 62) return -1;
    int i = f->top + (y - 62) / FB_ROW;
    return (y - 62) / FB_ROW < FB_ROWS && i < f->n ? i : -1;
}

static void files_mouse(window_t *w, int x, int y, int ev) {
    if (ev != MOUSE_DOWN) return;
    files_t *f = w->data;
    int i = files_row_at(f, x, y);
    if (i < 0) return;
    if (i == f->sel && f->ent[i].is_dir) files_open_sel(w, NULL);   /* second click opens a folder */
    else files_select(f, i);
}

static void files_context(window_t *w, int x, int y) {
    files_t *f = w->data;
    int i = files_row_at(f, x, y);
    if (i >= 0) files_select(f, i);
    bool has = f->sel >= 0, file = has && !f->ent[f->sel].is_dir;
    gui_menu_item_t items[] = {
        { file ? "Open in Notes" : "Open", files_open_sel, !has },
        { "Copy", files_copy, !file },
        { "Paste", files_paste, !file_clip[0] },
        { "Delete", files_delete, !has },
        { NULL, NULL, false },
        { "New folder", files_new_folder, false },
        { "Refresh", files_refresh, false },
    };
    gui_show_menu(items, 7, w, NULL);
}

static void files_key(window_t *w, int k) {
    files_t *f = w->data;
    switch (k & ~KEY_SHIFT) {
    case KEY_UP: files_select(f, f->sel < 0 ? 0 : f->sel - 1); break;
    case KEY_DOWN: files_select(f, f->sel + 1); break;
    case KEY_HOME: files_select(f, 0); break;
    case KEY_END: files_select(f, f->n - 1); break;
    case '\n': files_open_sel(w, NULL); break;
    case '\b': files_up(w, NULL); break;
    case KEY_DELETE: files_delete(w, NULL); break;
    case KEY_CTRL_C: files_copy(w, NULL); break;
    case KEY_CTRL_V: files_paste(w, NULL); break;
    }
}

static void files_paint(window_t *w, int cx, int cy) {
    files_t *f = w->data;
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    int mx, my, hover = -1;
    if (gui_mouse_in_client(w, &mx, &my)) hover = files_row_at(f, mx, my);

    gfx_round_fill(cx + 16, cy + 56, 270, 330, 12, th->card);
    for (int r = 0; r < FB_ROWS && f->top + r < f->n; r++) {
        int i = f->top + r, y = cy + 62 + r * FB_ROW;
        bool sel = i == f->sel;
        if (sel) gfx_round_fill(cx + 22, y, 258, FB_ROW - 2, 7, acc);
        else if (i == hover) gfx_round_fill(cx + 22, y, 258, FB_ROW - 2, 7, th->hover);
        uint32_t fg = sel ? 0xFFFFFF : th->text;
        if (f->ent[i].is_dir) {
            gfx_round_fill(cx + 30, y + 5, 8, 4, 1, 0xF5B83D);
            gfx_round_fill(cx + 30, y + 7, 16, 11, 2, 0xF5B83D);
        } else {
            gfx_round_fill(cx + 32, y + 4, 12, 15, 2, sel ? 0xFFFFFF : th->dim);
            gfx_round_fill(cx + 34, y + 6, 8, 11, 1, sel ? acc : th->card);
        }
        gfx_text(cx + 54, y + 3, f->ent[i].name, fg);
        if (!f->ent[i].is_dir) {
            char sz[16];
            snprintf(sz, sizeof(sz), "%u", f->ent[i].size);
            gfx_text(cx + 272 - gfx_text_width(sz), y + 3, sz, sel ? 0xFFFFFF : th->dim);
        }
    }
    if (f->n > FB_ROWS) {                                         /* scroll indicator */
        int bh = 318 * FB_ROWS / f->n, by = cy + 62 + 318 * f->top / f->n;
        gfx_round_fill(cx + 281, by, 3, bh, 1, th->dim);
    }

    int px = cx + 298, py = cy + 56, pw = 326, ph = 330;      /* preview pane */
    gfx_round_fill(px, py, pw, ph, 12, th->input);
    gfx_round_outline(px, py, pw, ph, 12, th->input_border, 255);
    int col = 0, row = 0, cols = (pw - 24) / FONT_W, rows = (ph - 20) / FONT_H;
    for (char *p = f->preview; *p && row < rows; p++) {
        if (*p == '\n' || col >= cols) { row++; col = 0; if (*p == '\n') continue; if (row >= rows) break; }
        if (*p == '\t' || *p == '\r') { col += *p == '\t'; continue; }
        gfx_char(px + 12 + col * FONT_W, py + 10 + row * FONT_H, *p, th->text, 0, true);
        col++;
    }
    if (!f->preview[0]) gfx_text(px + 12, py + 12, T("Select a file to preview it."), th->dim);
    gfx_set_clip(cx + 16, cy + 390, w->w - 32, 24);
    gfx_text(cx + 18, cy + 396, f->status, th->dim);
    gfx_reset_clip();
}

void app_files(void) {
    window_t *w = gui_window_create(APP_FILES, "Files", -1, -1, 640, 420);
    if (!w) return;
    files_t *f = kzalloc(sizeof(files_t));
    if (!f) { gui_window_close(w); return; }
    w->data = f;
    w->free_data = true;
    w->paint = files_paint;
    w->on_mouse = files_mouse;
    w->on_context = files_context;
    w->on_key = files_key;
    strlcpy(f->cwd, "/", sizeof(f->cwd));
    gui_add_widget(w, W_BUTTON, 16, 12, 64, 32, "Up")->on_click = files_up;
    gui_add_widget(w, W_BUTTON, 88, 12, 96, 32, "Refresh")->on_click = files_refresh;
    gui_add_widget(w, W_BUTTON, 192, 12, 118, 32, "New folder")->on_click = files_new_folder;
    f->path = gui_add_widget(w, W_LABEL, 324, 12, 300, 32, "/");
    f->path->style = S_HEADING;
    files_load(f);
    w->focus = -1;
}

/* ===== Settings ============================================================= */
typedef struct {
    widget_t *lang, *theme, *accent[NUM_ACCENTS], *wall[NUM_WALLPAPERS];
    widget_t *anim, *speed, *shadows, *transl, *seconds, *mouse, *mouse_lbl;
} settings_ui_t;

static void settings_sync(settings_ui_t *u) {
    gui_settings_t *s = &gui_settings;
    u->theme->value = s->theme;
    u->lang->value = s->lang;
    for (int i = 0; i < NUM_ACCENTS; i++) u->accent[i]->value = i == s->accent;
    for (int i = 0; i < NUM_WALLPAPERS; i++) u->wall[i]->value = i == s->wallpaper;
    u->anim->value = s->animations;
    u->speed->value = s->anim_speed;
    u->shadows->value = s->shadows;
    u->transl->value = s->translucent;
    u->seconds->value = s->seconds;
    u->mouse->value = s->mouse_speed;
    snprintf(u->mouse_lbl->text, sizeof(u->mouse_lbl->text), "%d%%", s->mouse_speed);
}

static void set_lang(window_t *w, widget_t *wd) { UNUSED(w); gui_settings.lang = wd->value; gui_settings_changed(true); }
static void set_theme(window_t *w, widget_t *wd) { UNUSED(w); gui_settings.theme = wd->value; gui_settings_changed(true); }
static void set_speed(window_t *w, widget_t *wd) { UNUSED(w); gui_settings.anim_speed = wd->value; gui_settings_changed(true); }

static void set_accent(window_t *w, widget_t *wd) {
    gui_settings.accent = wd->tag;
    settings_sync(w->data);
    gui_settings_changed(true);
}

static void set_wallpaper(window_t *w, widget_t *wd) {
    gui_settings.wallpaper = wd->tag;
    settings_sync(w->data);
    gui_settings_changed(true);
}

static void set_flag(window_t *w, widget_t *wd) {
    UNUSED(w);
    bool *flags[4] = { &gui_settings.animations, &gui_settings.shadows, &gui_settings.translucent, &gui_settings.seconds };
    *flags[wd->tag] = wd->value;
    gui_settings_changed(true);
}

static void set_mouse(window_t *w, widget_t *wd) {          /* live while dragging */
    settings_ui_t *u = w->data;
    gui_settings.mouse_speed = wd->value;
    snprintf(u->mouse_lbl->text, sizeof(u->mouse_lbl->text), "%d%%", wd->value);
    gui_settings_changed(false);
}

static void save_mouse(window_t *w, widget_t *wd) { UNUSED(w); UNUSED(wd); gui_settings_changed(true); }

static void reset_settings(window_t *w, widget_t *wd) {
    UNUSED(wd);
    gui_settings_defaults();
    settings_sync(w->data);
    gui_settings_changed(true);
    gui_toast("Settings restored to defaults");
}

static widget_t *settings_row(window_t *w, int y, const char *label) {
    return gui_add_widget(w, W_LABEL, 24, y, 170, 32, label);
}

static widget_t *settings_toggle(window_t *w, int y, const char *label, int tag) {
    settings_row(w, y, label);
    widget_t *t = gui_add_widget(w, W_TOGGLE, 452, y + 4, 44, 24, "");
    t->tag = tag;
    t->on_change = set_flag;
    return t;
}

void app_settings(void) {
    window_t *w = gui_window_create(APP_SETTINGS, "Settings", -1, -1, 520, 600);
    if (!w) return;
    settings_ui_t *u = kzalloc(sizeof(settings_ui_t));
    if (!u) { gui_window_close(w); return; }
    w->data = u;
    w->free_data = true;

    gui_add_widget(w, W_LABEL, 24, 14, 200, 20, "Appearance")->style = S_HEADING;
    settings_row(w, 44, "Theme");
    u->theme = gui_add_widget(w, W_CHOICE, 200, 44, 296, 32, "Dark|Light");
    u->theme->on_change = set_theme;
    settings_row(w, 88, "Language");
    u->lang = gui_add_widget(w, W_CHOICE, 200, 88, 296, 32, "English|Русский");
    u->lang->on_change = set_lang;
    settings_row(w, 134, "Accent color");
    for (int i = 0; i < NUM_ACCENTS; i++) {
        u->accent[i] = gui_add_widget(w, W_SWATCH, 200 + i * 44, 134, 32, 32, accent_names[i]);
        u->accent[i]->color = accent_colors[i];
        u->accent[i]->tag = i;
        u->accent[i]->on_click = set_accent;
    }
    settings_row(w, 184, "Wallpaper");
    for (int i = 0; i < NUM_WALLPAPERS; i++) {
        u->wall[i] = gui_add_widget(w, W_SWATCH, 200 + i * 60, 182, 50, 36, wallpaper_names[i]);
        u->wall[i]->style = S_WALLPAPER;
        u->wall[i]->tag = i;
        u->wall[i]->on_click = set_wallpaper;
    }

    gui_add_widget(w, W_LABEL, 24, 240, 200, 20, "Effects")->style = S_HEADING;
    u->anim = settings_toggle(w, 268, "Animations", 0);
    settings_row(w, 306, "Animation speed");
    u->speed = gui_add_widget(w, W_CHOICE, 200, 306, 296, 32, "Slow|Normal|Fast");
    u->speed->on_change = set_speed;
    u->shadows = settings_toggle(w, 348, "Window shadows", 1);
    u->transl = settings_toggle(w, 386, "Translucent panels", 2);

    gui_add_widget(w, W_LABEL, 24, 436, 200, 20, "Desktop")->style = S_HEADING;
    u->seconds = settings_toggle(w, 464, "Clock shows seconds", 3);
    settings_row(w, 504, "Mouse speed");
    u->mouse = gui_add_widget(w, W_SLIDER, 200, 506, 236, 28, "");
    u->mouse->min = 25;
    u->mouse->max = 300;
    u->mouse->on_change = set_mouse;
    u->mouse->on_click = save_mouse;
    u->mouse_lbl = gui_add_widget(w, W_LABEL, 448, 504, 60, 32, "");
    u->mouse_lbl->style = S_DIM;

    gui_add_widget(w, W_BUTTON, 24, 552, 200, 34, "Reset to defaults")->on_click = reset_settings;
    gui_add_widget(w, W_LABEL, 236, 552, 270, 34, "Saved to /etc/gui.cfg")->style = S_DIM;

    settings_sync(u);
    for (int i = 0; i < w->nwidgets; i++) w->widgets[i]->anim = w->widgets[i]->value * 1000;  /* no intro animation */
    w->focus = -1;
}

/* ===== Notes: a small text editor =========================================== */
#define NOTE_MAX 16000
#define NA_X     16                           /* text area inside the client */
#define NA_Y     54
#define NA_W     608
#define NA_H     360
#define NA_GUT   48                           /* line-number gutter */
#define NA_TX    (NA_X + NA_GUT + 8)
#define NA_ROWS  ((NA_H - 16) / FONT_H)
#define NA_COLS  ((NA_W - NA_GUT - 18) / FONT_W)

typedef struct {
    char buf[NOTE_MAX];
    int  len, cur, anchor;
    bool sel, modified;
    int  top, left;                           /* first visible line and column */
    widget_t *path;
} notes_t;

static int n_line_start(notes_t *n, int p) { while (p > 0 && n->buf[p - 1] != '\n') p--; return p; }
static int n_line_end(notes_t *n, int p) { while (p < n->len && n->buf[p] != '\n') p++; return p; }

static void n_pos_lc(notes_t *n, int pos, int *line, int *col) {
    int l = 0, s = 0;
    for (int i = 0; i < pos; i++) if (n->buf[i] == '\n') { l++; s = i + 1; }
    *line = l; *col = pos - s;
}

static int n_lc_pos(notes_t *n, int line, int col) {
    int p = 0, l = 0;
    if (line < 0) return 0;
    while (l < line && p < n->len) { if (n->buf[p] == '\n') l++; p++; }
    if (l < line) return n->len;
    return MIN(p + MAX(col, 0), n_line_end(n, p));
}

static int n_lines(notes_t *n) {
    int l = 1;
    for (int i = 0; i < n->len; i++) l += n->buf[i] == '\n';
    return l;
}

static void n_scroll(notes_t *n) {
    int line, col;
    n_pos_lc(n, n->cur, &line, &col);
    if (line < n->top) n->top = line;
    if (line >= n->top + NA_ROWS) n->top = line - NA_ROWS + 1;
    if (col < n->left) n->left = col;
    if (col >= n->left + NA_COLS) n->left = col - NA_COLS + 1;
}

static bool n_has_sel(notes_t *n) { return n->sel && n->anchor != n->cur; }
static void n_range(notes_t *n, int *a, int *b) { *a = MIN(n->anchor, n->cur); *b = MAX(n->anchor, n->cur); }

static void n_delete_sel(notes_t *n) {
    if (n_has_sel(n)) {
        int a, b;
        n_range(n, &a, &b);
        memmove(n->buf + a, n->buf + b, n->len - b);
        n->len -= b - a;
        n->cur = a;
        n->modified = true;
    }
    n->sel = false;
}

static void n_insert(notes_t *n, const char *s, int len) {
    n_delete_sel(n);
    for (int i = 0; i < len && n->len < NOTE_MAX - 1; i++) {
        char c = s[i];
        if (c == '\t') c = ' ';
        if (c != '\n' && (c < 32 || c > 126)) continue;
        memmove(n->buf + n->cur + 1, n->buf + n->cur, n->len - n->cur);
        n->buf[n->cur++] = c;
        n->len++;
        n->modified = true;
    }
    n_scroll(n);
}

static void n_copy(notes_t *n, bool cut) {
    if (!n_has_sel(n)) { gui_toast("Select some text first (Ctrl+A selects all)"); return; }
    int a, b;
    n_range(n, &a, &b);
    clipboard_set(n->buf + a, b - a);
    if (cut) n_delete_sel(n);
    gui_toast(cut ? "Cut to clipboard" : "Copied to clipboard");
}

static void n_paste(notes_t *n) {
    char *tmp = kmalloc(CLIPBOARD_MAX);
    if (!tmp) return;
    int len = clipboard_get(tmp, CLIPBOARD_MAX);
    if (!len) gui_toast("Clipboard is empty");
    else n_insert(n, tmp, len);
    kfree(tmp);
}

static void notes_set_title(window_t *w) {
    notes_t *n = w->data;
    const char *name = strrchr(n->path->text, '/');
    name = name ? name + 1 : n->path->text;
    if (name[0]) snprintf(w->title, sizeof(w->title), "%s - %s", T("Notes"), name);
    else strlcpy(w->title, "Notes", sizeof(w->title));
}

static void notes_load(window_t *w, widget_t *wd) {
    UNUSED(wd);
    notes_t *n = w->data;
    char path[128], msg[96];
    if (!n->path->text[0]) { gui_toast("Type a file name, e.g. /home/notes.txt"); return; }
    path_normalize("/", n->path->text, path, sizeof(path));
    int r = fs_read(path, 0, n->buf, NOTE_MAX - 1);
    if (r < 0) { snprintf(msg, sizeof(msg), "%s: %s", path, fs_strerror(r)); gui_toast(msg); return; }
    n->len = r; n->cur = 0; n->top = 0; n->left = 0; n->sel = false; n->modified = false;
    strlcpy(n->path->text, path, sizeof(n->path->text));
    n->path->cursor = strlen(path);
    notes_set_title(w);
    snprintf(msg, sizeof(msg), T("Opened %s"), path);
    gui_toast(msg);
}

static void notes_save(window_t *w, widget_t *wd) {
    UNUSED(wd);
    notes_t *n = w->data;
    char path[128], msg[96];
    if (!n->path->text[0]) { gui_toast("Type a file name, e.g. /home/notes.txt"); return; }
    path_normalize("/", n->path->text, path, sizeof(path));
    int r = fs_write(path, n->buf, n->len, false);
    if (r < 0) snprintf(msg, sizeof(msg), "%s: %s", path, fs_strerror(r));
    else { snprintf(msg, sizeof(msg), T("Saved %d bytes to %s"), r, path); n->modified = false; }
    strlcpy(n->path->text, path, sizeof(n->path->text));
    n->path->cursor = strlen(path);
    notes_set_title(w);
    gui_toast(msg);
}

static void notes_new(window_t *w, widget_t *wd) {
    UNUSED(wd);
    notes_t *n = w->data;
    n->len = n->cur = n->top = n->left = 0;
    n->sel = n->modified = false;
    n->path->text[0] = 0;
    n->path->cursor = 0;
    notes_set_title(w);
}

static void m_n_cut(window_t *w, widget_t *wd) { UNUSED(wd); n_copy(w->data, true); }
static void m_n_copy(window_t *w, widget_t *wd) { UNUSED(wd); n_copy(w->data, false); }
static void m_n_paste(window_t *w, widget_t *wd) { UNUSED(wd); n_paste(w->data); }
static void m_n_all(window_t *w, widget_t *wd) {
    UNUSED(wd);
    notes_t *n = w->data;
    n->sel = true; n->anchor = 0; n->cur = n->len;
}

static void notes_key(window_t *w, int k) {
    notes_t *n = w->data;
    bool shift = k & KEY_SHIFT;
    int c = k & ~KEY_SHIFT, line, col;
    n_pos_lc(n, n->cur, &line, &col);
    bool move = c == KEY_LEFT || c == KEY_RIGHT || c == KEY_UP || c == KEY_DOWN ||
                c == KEY_HOME || c == KEY_END || c == KEY_PGUP || c == KEY_PGDN;
    if (move) {
        if (shift && !n->sel) { n->sel = true; n->anchor = n->cur; }
        if (!shift && n_has_sel(n) && (c == KEY_LEFT || c == KEY_RIGHT)) {
            int a, b;
            n_range(n, &a, &b);
            n->cur = c == KEY_LEFT ? a : b;
            n->sel = false;
            n_scroll(n);
            return;
        }
        if (!shift) n->sel = false;
        switch (c) {
        case KEY_LEFT: if (n->cur > 0) n->cur--; break;
        case KEY_RIGHT: if (n->cur < n->len) n->cur++; break;
        case KEY_UP: if (line > 0) n->cur = n_lc_pos(n, line - 1, col); break;
        case KEY_DOWN: if (line + 1 < n_lines(n)) n->cur = n_lc_pos(n, line + 1, col); break;
        case KEY_HOME: n->cur = n_line_start(n, n->cur); break;
        case KEY_END: n->cur = n_line_end(n, n->cur); break;
        case KEY_PGUP: n->cur = n_lc_pos(n, MAX(0, line - NA_ROWS), col); break;
        case KEY_PGDN: n->cur = n_lc_pos(n, line + NA_ROWS, col); break;
        }
        n_scroll(n);
        return;
    }
    switch (c) {
    case KEY_CTRL_A: m_n_all(w, NULL); return;
    case KEY_CTRL_C: n_copy(n, false); return;
    case KEY_CTRL_X: n_copy(n, true); return;
    case KEY_CTRL_V: n_paste(n); return;
    case KEY_CTRL_S: notes_save(w, NULL); return;
    case '\b':
        if (n_has_sel(n)) n_delete_sel(n);
        else if (n->cur > 0) { n->anchor = n->cur - 1; n->sel = true; n_delete_sel(n); }
        n_scroll(n);
        return;
    case KEY_DELETE:
        if (n_has_sel(n)) n_delete_sel(n);
        else if (n->cur < n->len) { n->anchor = n->cur + 1; n->sel = true; n_delete_sel(n); }
        return;
    }
    if (c == '\n' || (c >= 32 && c < 127)) { char ch = (char)c; n_insert(n, &ch, 1); }
}

static int n_pos_at(notes_t *n, int x, int y) {
    int line = n->top + (y - NA_Y - 8 + FONT_H) / FONT_H - 1;     /* rounds toward -inf above the area */
    int col = n->left + (x - NA_TX + FONT_W / 2) / FONT_W;
    return n_lc_pos(n, MAX(0, line), col);
}

static void notes_mouse(window_t *w, int x, int y, int ev) {
    notes_t *n = w->data;
    if (ev == MOUSE_DOWN && !inside_rect(x, y, NA_X, NA_Y, NA_W, NA_H)) return;
    int p = n_pos_at(n, x, y);
    if (ev == MOUSE_DOWN) { n->cur = n->anchor = p; n->sel = true; }
    else if (ev == MOUSE_DRAG) { n->cur = p; n_scroll(n); }
    else if (n->anchor == n->cur) n->sel = false;
    gui_invalidate();
}

static void notes_context(window_t *w, int x, int y) {
    UNUSED(x); UNUSED(y);
    notes_t *n = w->data;
    bool sel = n_has_sel(n);
    gui_menu_item_t items[] = {
        { "Cut", m_n_cut, !sel }, { "Copy", m_n_copy, !sel },
        { "Paste", m_n_paste, clipboard_len() == 0 },
        { NULL, NULL, false },
        { "Select all", m_n_all, n->len == 0 },
        { "Save", notes_save, false },
    };
    gui_show_menu(items, 6, w, NULL);
}

static void notes_paint(window_t *w, int cx, int cy) {
    notes_t *n = w->data;
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    bool focus = gui_is_focused(w) && w->focus < 0;
    int ax = cx + NA_X, ay = cy + NA_Y;
    gfx_round_fill(ax, ay, NA_W, NA_H, 10, th->input);
    gfx_round_outline(ax, ay, NA_W, NA_H, 10, focus ? acc : th->input_border, 255);
    gfx_fill(ax + NA_GUT, ay + 6, 1, NA_H - 12, th->border);
    int sa = 0, sb = 0, cline, ccol;
    if (n_has_sel(n)) n_range(n, &sa, &sb);
    n_pos_lc(n, n->cur, &cline, &ccol);
    gfx_set_clip(ax + 2, ay + 2, NA_W - 4, NA_H - 4);
    int p = n_lc_pos(n, n->top, 0);
    char num[12];
    for (int row = 0; row < NA_ROWS; row++) {
        int line = n->top + row, y = ay + 8 + row * FONT_H, e = n_line_end(n, p);
        snprintf(num, sizeof(num), "%d", line + 1);
        gfx_text(ax + NA_GUT - 8 - gfx_text_width(num), y, num, line == cline ? th->text : th->dim);
        for (int i = p; i <= e; i++) {
            int col = i - p;
            if (col < n->left) continue;
            if (col >= n->left + NA_COLS) break;
            int x = cx + NA_TX + (col - n->left) * FONT_W;
            if (i >= sa && i < sb) gfx_fill(x, y, FONT_W, FONT_H, gfx_mix(th->input, acc, 110));
            if (i < e) gfx_char(x, y, n->buf[i], th->text, 0, true);
        }
        if (focus && line == cline && gui_blink_on() && ccol >= n->left && ccol < n->left + NA_COLS)
            gfx_fill(cx + NA_TX + (ccol - n->left) * FONT_W, y - 1, 2, FONT_H + 2, acc);
        if (e >= n->len) break;
        p = e + 1;
    }
    gfx_reset_clip();
    char st[96];
    snprintf(st, sizeof(st), T("Line %d, column %d   |   %d characters"), cline + 1, ccol + 1, n->len);
    gfx_text(cx + NA_X + 4, cy + NA_Y + NA_H + 12, st, th->dim);
    if (n->modified) {
        const char *m = T("modified - Ctrl+S saves");
        gfx_text(cx + NA_X + NA_W - 4 - gfx_text_width(m), cy + NA_Y + NA_H + 12, m, acc);
    }
}

static window_t *notes_create(void) {
    window_t *w = gui_window_create(APP_NOTES, "Notes", -1, -1, 640, 446);
    if (!w) return NULL;
    notes_t *n = kzalloc(sizeof(notes_t));
    if (!n) { gui_window_close(w); return NULL; }
    w->data = n;
    w->free_data = true;
    w->paint = notes_paint;
    w->on_key = notes_key;
    w->on_mouse = notes_mouse;
    w->on_context = notes_context;
    n->path = gui_add_widget(w, W_TEXTBOX, 16, 10, 300, 34, "");
    strlcpy(n->path->placeholder, "/home/notes.txt", sizeof(n->path->placeholder));
    n->path->on_enter = notes_load;
    gui_add_widget(w, W_BUTTON, 324, 10, 84, 34, "Open")->on_click = notes_load;
    widget_t *b = gui_add_widget(w, W_BUTTON, 416, 10, 84, 34, "Save");
    b->style = S_PRIMARY;
    b->on_click = notes_save;
    gui_add_widget(w, W_BUTTON, 508, 10, 116, 34, "New")->on_click = notes_new;
    w->focus = -1;                                 /* start typing into the text area */
    return w;
}

void app_notes(void) { notes_create(); }

void app_notes_open(const char *path) {
    window_t *w = notes_create();
    if (!w) return;
    notes_t *n = w->data;
    strlcpy(n->path->text, path, sizeof(n->path->text));
    notes_load(w, NULL);
}

/* ===== Snake ================================================================ */
#define SN_W    24
#define SN_H    17
#define SN_CELL 18
#define SN_X    16
#define SN_Y    48
typedef struct {
    uint8_t x[SN_W * SN_H], y[SN_W * SN_H];
    int  len, dir, next, fx, fy, score;
    bool started, over, paused;
    uint32_t last, seed;
} snake_t;
static int snake_best;
static const int sn_dx[4] = { 1, 0, -1, 0 }, sn_dy[4] = { 0, 1, 0, -1 };

static int sn_rand(snake_t *s) { s->seed = s->seed * 1103515245u + 12345u; return (s->seed >> 16) & 0x7FFF; }

static bool sn_on_snake(snake_t *s, int x, int y, int from) {
    for (int i = from; i < s->len; i++) if (s->x[i] == x && s->y[i] == y) return true;
    return false;
}

static void sn_food(snake_t *s) {
    for (int tries = 0; tries < 2000; tries++) {
        s->fx = sn_rand(s) % SN_W;
        s->fy = sn_rand(s) % SN_H;
        if (!sn_on_snake(s, s->fx, s->fy, 0)) return;
    }
}

static void sn_reset(snake_t *s) {
    s->len = 4;
    for (int i = 0; i < s->len; i++) { s->x[i] = SN_W / 2 - i; s->y[i] = SN_H / 2; }
    s->dir = s->next = 0;
    s->score = 0;
    s->over = s->paused = false;
    s->started = true;
    s->seed ^= timer_ticks();
    sn_food(s);
}

static void snake_tick(window_t *w) {
    snake_t *s = w->data;
    if (!s->started || s->over || s->paused) return;
    uint32_t interval = 150 - MIN(80, s->score / 2);
    if (timer_ticks() - s->last < interval) return;
    s->last = timer_ticks();
    s->dir = s->next;
    int hx = s->x[0] + sn_dx[s->dir], hy = s->y[0] + sn_dy[s->dir];
    if (hx < 0 || hy < 0 || hx >= SN_W || hy >= SN_H || sn_on_snake(s, hx, hy, 0)) {
        s->over = true;
        if (s->score > snake_best) { snake_best = s->score; gui_toast("New record!"); }
        gui_invalidate();
        return;
    }
    int tx = s->x[s->len - 1], ty = s->y[s->len - 1];
    for (int i = s->len - 1; i > 0; i--) { s->x[i] = s->x[i - 1]; s->y[i] = s->y[i - 1]; }
    s->x[0] = hx; s->y[0] = hy;
    if (hx == s->fx && hy == s->fy) {
        if (s->len < SN_W * SN_H) { s->x[s->len] = tx; s->y[s->len] = ty; s->len++; }
        s->score += 10;
        sn_food(s);
    }
    gui_invalidate();
}

static void snake_key(window_t *w, int k) {
    snake_t *s = w->data;
    int c = k & ~KEY_SHIFT, d = -1;
    if (c == KEY_RIGHT || c == 'd') d = 0;
    if (c == KEY_DOWN || c == 's') d = 1;
    if (c == KEY_LEFT || c == 'a') d = 2;
    if (c == KEY_UP || c == 'w') d = 3;
    if (d >= 0 && s->started && !s->over && d != (s->dir + 2) % 4) s->next = d;
    if (c == ' ' || c == '\n') {
        if (!s->started || s->over) sn_reset(s);
        else s->paused = !s->paused;
    }
    if (c == 'p' && s->started && !s->over) s->paused = !s->paused;
}

static void snake_paint(window_t *w, int cx, int cy) {
    snake_t *s = w->data;
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    char buf[48];
    snprintf(buf, sizeof(buf), T("Score: %d"), s->score);
    gfx_text_bold(cx + SN_X, cy + 16, buf, th->text);
    snprintf(buf, sizeof(buf), T("Best: %d"), snake_best);
    gfx_text(cx + SN_X + SN_W * SN_CELL - gfx_text_width(buf), cy + 16, buf, th->dim);
    int bx = cx + SN_X, by = cy + SN_Y, bw = SN_W * SN_CELL, bh = SN_H * SN_CELL;
    gfx_round_fill(bx - 4, by - 4, bw + 8, bh + 8, 12, th->card);
    for (int y = 0; y < SN_H; y++)                                   /* subtle checkerboard */
        for (int x = (y & 1); x < SN_W; x += 2)
            gfx_fill(bx + x * SN_CELL, by + y * SN_CELL, SN_CELL, SN_CELL, gfx_mix(th->card, th->text, 8));
    if (s->started) {
        gfx_circle(bx + s->fx * SN_CELL + SN_CELL / 2, by + s->fy * SN_CELL + SN_CELL / 2, SN_CELL / 2 - 2, 0xFF5A5A, 255);
        for (int i = s->len - 1; i >= 0; i--) {
            uint32_t c = i == 0 ? gfx_mix(acc, 0xFFFFFF, 60) : gfx_mix(acc, th->card, MIN(140, i * 6));
            gfx_round_fill(bx + s->x[i] * SN_CELL + 1, by + s->y[i] * SN_CELL + 1, SN_CELL - 2, SN_CELL - 2, 5, c);
        }
    }
    const char *msg = !s->started ? "Press Space to start" : s->over ? "Game over - Space to play again" :
                      s->paused ? "Paused - Space to continue" : NULL;
    if (msg) {
        gfx_round_blend(bx + bw / 2 - 170, by + bh / 2 - 40, 340, 80, 14, th->menu, 230);
        msg = T(msg);
        gfx_text_bold(bx + (bw - gfx_text_width(msg)) / 2, by + bh / 2 - 18, msg, th->text);
        const char *hint = T("Arrows or WASD to steer");
        gfx_text(bx + (bw - gfx_text_width(hint)) / 2, by + bh / 2 + 4, hint, th->dim);
    }
}

void app_snake(void) {
    window_t *w = gui_window_create(APP_SNAKE, "Snake", -1, -1, SN_W * SN_CELL + 2 * SN_X, SN_Y + SN_H * SN_CELL + 16);
    if (!w) return;
    snake_t *s = kzalloc(sizeof(snake_t));
    if (!s) { gui_window_close(w); return; }
    s->seed = timer_ticks() | 1;
    w->data = s;
    w->free_data = true;
    w->paint = snake_paint;
    w->on_key = snake_key;
    w->on_tick = snake_tick;
}
