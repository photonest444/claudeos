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

/* ===== Terminal: a termbuf + tty + its own shell task ====================== */
#define TERM_COLS 80
#define TERM_ROWS 25
#define TERM_PX   12
#define TERM_PY   10

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

static void term_paint(window_t *w, int cx, int cy) {
    tty_t *tty = w->data;
    gfx_fill(cx, cy, w->w, w->h - TITLE_H, vga_palette[0]);
    if (!tty) return;
    termbuf_t *tb = tty->tb;
    for (int y = 0; y < tb->rows; y++)
        for (int x = 0; x < tb->cols; x++) {
            uint16_t v = tb->cells[y * tb->cols + x];
            uint8_t ch = v & 0xFF, attr = v >> 8;
            if (ch == ' ' && !(attr >> 4)) continue;          /* background already drawn */
            gfx_char(cx + TERM_PX + x * FONT_W, cy + TERM_PY + y * FONT_H, (char)ch,
                     vga_palette[attr & 15], vga_palette[attr >> 4], false);
        }
    if (tty->hangup) return;
    int px = cx + TERM_PX + tb->cx * FONT_W, py = cy + TERM_PY + tb->cy * FONT_H;
    if (!gui_is_focused(w)) gfx_rect(px, py, FONT_W, FONT_H, vga_palette[8]);
    else if (gui_blink_on()) gfx_fill(px, py + FONT_H - 3, FONT_W, 3, gui_accent());
}

static void term_key(window_t *w, int k) { if (w->data) tty_input(w->data, k); }

static void term_close(window_t *w) {
    tty_t *tty = w->data;
    if (!tty) return;
    tty->hangup = 1;                          /* the shell sees this and exits */
    if (tty->fg_pid) task_kill(tty->fg_pid);
    w->data = NULL;
    term_release(tty);
}

static void term_tick(window_t *w) {
    tty_t *tty = w->data;
    if (tty && tty->hangup) gui_window_close(w);   /* shell exited ('exit') */
}

void app_terminal(void) {
    tty_t *tty = kzalloc(sizeof(tty_t));
    termbuf_t *tb = kzalloc(sizeof(termbuf_t));
    uint16_t *cells = kmalloc(TERM_COLS * TERM_ROWS * 2);
    if (!tty || !tb || !cells) { kfree(tty); kfree(tb); kfree(cells); return; }
    tb->on_cell = term_hook_cell;
    tb->on_redraw = term_hook_any;
    tb->on_scroll = term_hook_any;
    tb->on_cursor = term_hook_cursor;
    termbuf_init(tb, TERM_COLS, TERM_ROWS, cells);
    tty_init(tty, tb);
    tty->refs = 2;
    window_t *w = gui_window_create(APP_TERMINAL, "Terminal", -1, -1,
                                    TERM_COLS * FONT_W + 2 * TERM_PX, TERM_ROWS * FONT_H + 2 * TERM_PY);
    if (!w) { kfree(tty); kfree(tb); kfree(cells); return; }
    w->data = tty;
    w->paint = term_paint;
    w->on_key = term_key;
    w->on_close = term_close;
    w->on_tick = term_tick;
    int pid = task_create_kernel("shell", shell_main, tty, tty);
    if (pid < 0) { tty->refs = 1; gui_window_close(w); return; }
    snprintf(w->title, sizeof(w->title), "Terminal (pid %d)", pid);
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
    if (d->name->text[0]) snprintf(d->result->text, sizeof(d->result->text), "Hello, %s! Welcome to ClaudeOS.", d->name->text);
    else strlcpy(d->result->text, "Please type your name first.", sizeof(d->result->text));
}

static void demo_click(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    d->clicks++;
    snprintf(d->count->text, sizeof(d->count->text), "Clicked %d time%s", d->clicks, d->clicks == 1 ? "" : "s");
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
    snprintf(buf, sizeof(buf), "Size set to %s", names[wd->value % 3]);
    gui_toast(buf);
}

static void demo_save(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    char buf[96];
    int r = fs_write("/note.txt", d->note->text, strlen(d->note->text), false);
    if (r < 0) snprintf(buf, sizeof(buf), "Save failed: %s", fs_strerror(r));
    else snprintf(buf, sizeof(buf), "Saved %d bytes to /note.txt", r);
    gui_toast(buf);
}

static void demo_load(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    char buf[96];
    int r = fs_read("/note.txt", 0, d->note->text, sizeof(d->note->text) - 1);
    if (r < 0) { snprintf(buf, sizeof(buf), "Load failed: %s", fs_strerror(r)); gui_toast(buf); return; }
    d->note->text[r] = 0;
    for (int i = 0; i < r; i++) if (d->note->text[i] == '\n') d->note->text[i] = ' ';
    d->note->cursor = r;
    snprintf(buf, sizeof(buf), "Loaded %d bytes from /note.txt", r);
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
    d->count = gui_add_widget(w, W_LABEL, 150, 140, 200, 34, "Clicked 0 times");
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
        card(x + i * (cw + 12), cy + 14, cw, 74, titles[i]);
        gfx_text_scaled(x + i * (cw + 12) + 14, cy + 44, vals[i], i == 1 ? acc : th->text, 2);
    }

    /* CPU history graph */
    int gy = cy + 102;
    card(x, gy, W, 140, "CPU usage, last 30 s");
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
    snprintf(buf, sizeof(buf), "RAM  %u / %u MB", (tot - fr) / 1024, tot / 1024);
    gfx_text(x + 14, my + 14, buf, th->text);
    bar(x + 14, my + 44, half, tot - fr, tot);
    snprintf(buf, sizeof(buf), "Heap  %u / %u KB", hu / 1024, ht / 1024);
    gfx_text(x + 28 + half, my + 14, buf, th->text);
    bar(x + 28 + half, my + 44, half, hu, ht);

    /* task list */
    int ty = my + 88;
    card(x, ty, W, w->h - TITLE_H - (ty - cy) - 14, NULL);
    gfx_text(x + 14, ty + 10, "PID  STATE      CPU(ms)  TYPE    NAME", th->dim);
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
        "Version 1.1 for i386",
        "",
        "C + NASM kernel, preemptive multitasking,",
        "FAT12/16, ring-3 programs and this desktop.",
        "Font: Terminus (SIL Open Font License)",
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        gfx_text(mid - (int)strlen(lines[i]) * FONT_W / 2, cy + 156 + i * 18, lines[i], i == 0 ? acc : th->dim);
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
#define FB_MAX 64
#define FB_ROW 24
typedef struct {
    char cwd[128];
    fs_entry_t ent[FB_MAX];
    int n, sel;
    char preview[1600];
    char status[80];
    widget_t *path;
} files_t;

static void files_load(files_t *f) {
    f->n = fs_list(f->cwd, f->ent, FB_MAX);
    if (f->n < 0) { snprintf(f->status, sizeof(f->status), "%s", fs_strerror(f->n)); f->n = 0; }
    else snprintf(f->status, sizeof(f->status), "%d item(s)", f->n);
    f->sel = -1;
    f->preview[0] = 0;
    strlcpy(f->path->text, f->cwd, sizeof(f->path->text));
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

static int files_row_at(files_t *f, int x, int y) {
    if (x < 22 || x > 280 || y < 62) return -1;
    int i = (y - 62) / FB_ROW;
    return i < f->n && i < 13 ? i : -1;
}

static void files_mouse(window_t *w, int x, int y) {
    files_t *f = w->data;
    int i = files_row_at(f, x, y);
    if (i < 0) return;
    char p[128];
    path_normalize(f->cwd, f->ent[i].name, p, sizeof(p));
    if (f->ent[i].is_dir) { strlcpy(f->cwd, p, sizeof(f->cwd)); files_load(f); return; }
    f->sel = i;
    int r = fs_read(p, 0, f->preview, sizeof(f->preview) - 1);
    f->preview[r > 0 ? r : 0] = 0;
    bool binary = false;
    for (int k = 0; k < r; k++) if (!f->preview[k] || (uint8_t)f->preview[k] > 127) binary = true;
    if (binary) snprintf(f->preview, sizeof(f->preview), "[binary file, %u bytes]\n\nRun it from a terminal:\n  %s", f->ent[i].size, p);
    snprintf(f->status, sizeof(f->status), "%s - %u bytes", p, f->ent[i].size);
}

static void files_paint(window_t *w, int cx, int cy) {
    files_t *f = w->data;
    const theme_t *th = gui_theme();
    uint32_t acc = gui_accent();
    int mx, my, hover = -1;
    if (gui_mouse_in_client(w, &mx, &my)) hover = files_row_at(f, mx, my);

    gfx_round_fill(cx + 16, cy + 56, 270, 330, 12, th->card);
    for (int i = 0; i < f->n && i < 13; i++) {
        int y = cy + 62 + i * FB_ROW;
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
            gfx_text(cx + 272 - (int)strlen(sz) * FONT_W, y + 3, sz, sel ? 0xFFFFFF : th->dim);
        }
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
    if (!f->preview[0]) gfx_text(px + 12, py + 12, "Select a file to preview it.", th->dim);
    gfx_text(cx + 18, cy + 396, f->status, th->dim);
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
    strlcpy(f->cwd, "/", sizeof(f->cwd));
    gui_add_widget(w, W_BUTTON, 16, 12, 64, 32, "Up")->on_click = files_up;
    gui_add_widget(w, W_BUTTON, 88, 12, 90, 32, "Refresh")->on_click = files_refresh;
    f->path = gui_add_widget(w, W_LABEL, 192, 12, 430, 32, "/");
    f->path->style = S_HEADING;
    files_load(f);
}

/* ===== Settings ============================================================= */
typedef struct {
    widget_t *theme, *accent[NUM_ACCENTS], *wall[NUM_WALLPAPERS];
    widget_t *anim, *speed, *shadows, *transl, *seconds, *mouse, *mouse_lbl;
} settings_ui_t;

static void settings_sync(settings_ui_t *u) {
    gui_settings_t *s = &gui_settings;
    u->theme->value = s->theme;
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
    window_t *w = gui_window_create(APP_SETTINGS, "Settings", -1, -1, 520, 556);
    if (!w) return;
    settings_ui_t *u = kzalloc(sizeof(settings_ui_t));
    if (!u) { gui_window_close(w); return; }
    w->data = u;
    w->free_data = true;

    gui_add_widget(w, W_LABEL, 24, 14, 200, 20, "Appearance")->style = S_HEADING;
    settings_row(w, 44, "Theme");
    u->theme = gui_add_widget(w, W_CHOICE, 200, 44, 296, 32, "Dark|Light");
    u->theme->on_change = set_theme;
    settings_row(w, 90, "Accent color");
    for (int i = 0; i < NUM_ACCENTS; i++) {
        u->accent[i] = gui_add_widget(w, W_SWATCH, 200 + i * 44, 90, 32, 32, accent_names[i]);
        u->accent[i]->color = accent_colors[i];
        u->accent[i]->tag = i;
        u->accent[i]->on_click = set_accent;
    }
    settings_row(w, 140, "Wallpaper");
    for (int i = 0; i < NUM_WALLPAPERS; i++) {
        u->wall[i] = gui_add_widget(w, W_SWATCH, 200 + i * 60, 138, 50, 36, wallpaper_names[i]);
        u->wall[i]->style = S_WALLPAPER;
        u->wall[i]->tag = i;
        u->wall[i]->on_click = set_wallpaper;
    }

    gui_add_widget(w, W_LABEL, 24, 196, 200, 20, "Effects")->style = S_HEADING;
    u->anim = settings_toggle(w, 224, "Animations", 0);
    settings_row(w, 262, "Animation speed");
    u->speed = gui_add_widget(w, W_CHOICE, 200, 262, 296, 32, "Slow|Normal|Fast");
    u->speed->on_change = set_speed;
    u->shadows = settings_toggle(w, 304, "Window shadows", 1);
    u->transl = settings_toggle(w, 342, "Translucent panels", 2);

    gui_add_widget(w, W_LABEL, 24, 392, 200, 20, "Desktop")->style = S_HEADING;
    u->seconds = settings_toggle(w, 420, "Clock shows seconds", 3);
    settings_row(w, 460, "Mouse speed");
    u->mouse = gui_add_widget(w, W_SLIDER, 200, 462, 236, 28, "");
    u->mouse->min = 25;
    u->mouse->max = 300;
    u->mouse->on_change = set_mouse;
    u->mouse->on_click = save_mouse;
    u->mouse_lbl = gui_add_widget(w, W_LABEL, 448, 460, 60, 32, "");
    u->mouse_lbl->style = S_DIM;

    gui_add_widget(w, W_BUTTON, 24, 508, 170, 34, "Reset to defaults")->on_click = reset_settings;
    gui_add_widget(w, W_LABEL, 210, 508, 290, 34, "Saved automatically to /etc/gui.cfg")->style = S_DIM;

    settings_sync(u);
    for (int i = 0; i < w->nwidgets; i++) w->widgets[i]->anim = w->widgets[i]->value * 1000;  /* no intro animation */
    w->focus = -1;
}
