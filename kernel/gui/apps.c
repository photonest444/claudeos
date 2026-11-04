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

static int cascade;
static void next_pos(int *x, int *y) {
    *x = 130 + cascade * 28;
    *y = 30 + cascade * 24;
    cascade = (cascade + 1) % 6;
}

/* ===== Terminal: a termbuf + tty + its own shell task ====================== */
#define TERM_COLS 80
#define TERM_ROWS 25

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
    if (!tty) return;
    termbuf_t *tb = tty->tb;
    for (int y = 0; y < tb->rows; y++)
        for (int x = 0; x < tb->cols; x++) {
            uint16_t v = tb->cells[y * tb->cols + x];
            uint8_t ch = v & 0xFF, attr = v >> 8;
            if (ch == ' ' && !(attr >> 4)) continue;          /* background already black */
            gfx_char(cx + 2 + x * FONT_W, cy + 2 + y * FONT_H, (char)ch,
                     vga_palette[attr & 15], vga_palette[attr >> 4], false);
        }
    if (!tty->hangup)
        gfx_fill(cx + 2 + tb->cx * FONT_W, cy + 2 + tb->cy * FONT_H + FONT_H - 4, FONT_W, 3, 0xC0C0C0);
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
    int x, y;
    next_pos(&x, &y);
    window_t *w = gui_window_create("Terminal", x, y, TERM_COLS * FONT_W + 4, TERM_ROWS * FONT_H + 4);
    if (!w) { kfree(tty); kfree(tb); kfree(cells); return; }
    w->bg = 0x000000;
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
    gfx_fill(cx + 8, cy + 8, 200, 32, 0xE8F4E0);
    gfx_bevel(cx + 8, cy + 8, 200, 32, false);
    char buf[24];
    if (c->error) strlcpy(buf, "Error", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%d", c->cur);
    gfx_text(cx + 200 - (int)strlen(buf) * FONT_W, cy + 16, buf, 0x000000);
    if (c->op) gfx_char(cx + 14, cy + 16, c->op, 0x808080, 0, true);
}

static void calc_click(window_t *w, widget_t *wd) { calc_press(w->data, wd->text[0]); }

static void calc_key(window_t *w, int k) {
    if (k == '\n') k = '=';
    if (k == '\b' || k == 'c' || k == 27) k = 'C';
    if ((k >= '0' && k <= '9') || (k && strchr("+-*/=C", k))) calc_press(w->data, (char)k);
}

void app_calculator(void) {
    int x, y;
    next_pos(&x, &y);
    window_t *w = gui_window_create("Calculator", x, y, 216, 228);
    if (!w) return;
    w->data = kzalloc(sizeof(calc_t));
    w->free_data = true;
    w->paint = calc_paint;
    w->on_key = calc_key;
    static const char *keys = "789/456*123-C0=+";
    for (int i = 0; i < 16; i++) {
        char label[2] = { keys[i], 0 };
        widget_t *b = gui_add_widget(w, W_BUTTON, 8 + (i % 4) * 51, 50 + (i / 4) * 43, 47, 38, label);
        b->on_click = calc_click;
        if (strchr("/*-+=", keys[i])) b->color = 0xD8D8E8;
    }
}

/* ===== Widgets demo: labels, text boxes, buttons, file save/load ========== */
typedef struct { widget_t *name, *result, *count, *note, *status; int clicks; } demo_t;

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

static void demo_save(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    int r = fs_write("/note.txt", d->note->text, strlen(d->note->text), false);
    if (r < 0) snprintf(d->status->text, sizeof(d->status->text), "Save failed: %s", fs_strerror(r));
    else snprintf(d->status->text, sizeof(d->status->text), "Saved %d bytes to /note.txt (try: cat /note.txt)", r);
}

static void demo_load(window_t *w, widget_t *wd) {
    UNUSED(wd);
    demo_t *d = w->data;
    int r = fs_read("/note.txt", 0, d->note->text, sizeof(d->note->text) - 1);
    if (r < 0) { snprintf(d->status->text, sizeof(d->status->text), "Load failed: %s", fs_strerror(r)); return; }
    d->note->text[r] = 0;
    for (int i = 0; i < r; i++) if (d->note->text[i] == '\n') d->note->text[i] = ' ';
    d->note->cursor = r;
    snprintf(d->status->text, sizeof(d->status->text), "Loaded %d bytes from /note.txt", r);
}

void app_widgets_demo(void) {
    int x, y;
    next_pos(&x, &y);
    window_t *w = gui_window_create("Widgets Demo", x, y, 420, 230);
    if (!w) return;
    demo_t *d = kzalloc(sizeof(demo_t));
    w->data = d;
    w->free_data = true;
    gui_add_widget(w, W_LABEL, 10, 10, 90, 22, "Your name:");
    d->name = gui_add_widget(w, W_TEXTBOX, 100, 10, 210, 22, "");
    d->name->on_enter = demo_greet;
    gui_add_widget(w, W_BUTTON, 320, 8, 90, 26, "Greet")->on_click = demo_greet;
    d->result = gui_add_widget(w, W_LABEL, 10, 40, 400, 20, "Type a name and press Enter or Greet.");
    d->result->color = 0x000080;
    gui_add_widget(w, W_BUTTON, 10, 72, 100, 26, "Click me!")->on_click = demo_click;
    d->count = gui_add_widget(w, W_LABEL, 122, 74, 200, 22, "Clicked 0 times");
    gui_add_widget(w, W_LABEL, 10, 120, 50, 22, "Note:");
    d->note = gui_add_widget(w, W_TEXTBOX, 60, 120, 250, 22, "");
    gui_add_widget(w, W_BUTTON, 320, 118, 90, 26, "Save")->on_click = demo_save;
    gui_add_widget(w, W_BUTTON, 320, 150, 90, 26, "Load")->on_click = demo_load;
    d->status = gui_add_widget(w, W_LABEL, 10, 196, 400, 20, "Tab moves focus between widgets.");
    d->status->color = 0x404040;
}

/* ===== System monitor ======================================================= */
static void bar(int x, int y, int w, uint32_t used, uint32_t total, uint32_t color) {
    gfx_fill(x, y, w, 14, 0xFFFFFF);
    gfx_bevel(x, y, w, 14, false);
    uint32_t u = used >> 8, t = total >> 8;          /* scale down: no 64-bit division */
    int fill = t ? (int)(u * (uint32_t)(w - 4) / t) : 0;
    gfx_fill(x + 2, y + 2, fill, 10, color);
}

static void sysmon_paint(window_t *w, int cx, int cy) {
    UNUSED(w);
    char buf[96];
    uint32_t s = timer_ticks() / 1000;
    snprintf(buf, sizeof(buf), "Uptime: %u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    gfx_text(cx + 10, cy + 8, buf, 0);
    uint32_t tot = pmm_total_frames() * 4, fr = pmm_free_frames() * 4;
    snprintf(buf, sizeof(buf), "RAM:  %u / %u KB", tot - fr, tot);
    gfx_text(cx + 10, cy + 32, buf, 0);
    bar(cx + 220, cy + 33, 180, tot - fr, tot, 0x3060C0);
    uint32_t ht, hu, hb;
    heap_stats(&ht, &hu, &hb);
    snprintf(buf, sizeof(buf), "Heap: %u / %u KB", hu / 1024, ht / 1024);
    gfx_text(cx + 10, cy + 52, buf, 0);
    bar(cx + 220, cy + 53, 180, hu, ht, 0x30A050);
    gfx_fill(cx + 8, cy + 78, 396, 20, 0x0A246A);
    gfx_text(cx + 12, cy + 80, "PID  STATE     CPU(ms)  TYPE    NAME", 0xFFFFFF);
    int row = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_by_index(i);
        if (t->state == T_UNUSED) continue;
        snprintf(buf, sizeof(buf), "%3d  %-8s %8u  %-6s  %s", t->pid, task_state_name(t->state),
                 t->cpu_ticks, t->is_user ? "user" : "kernel", t->name);
        gfx_text(cx + 12, cy + 102 + row * 18, buf, t->is_user ? 0x804000 : 0x000000);
        row++;
    }
}

static void sysmon_tick(window_t *w) {
    uint32_t *last = w->data;
    if (timer_ticks() - *last >= 500) { *last = timer_ticks(); gui_invalidate(); }
}

void app_sysmon(void) {
    int x, y;
    next_pos(&x, &y);
    window_t *w = gui_window_create("System Monitor", x, y, 412, 360);
    if (!w) return;
    w->data = kzalloc(sizeof(uint32_t));
    w->free_data = true;
    w->paint = sysmon_paint;
    w->on_tick = sysmon_tick;
}

/* ===== About ================================================================ */
static void about_ok(window_t *w, widget_t *wd) { UNUSED(wd); gui_window_close(w); }

static void about_paint(window_t *w, int cx, int cy) {
    UNUSED(w);
    static const char *lines[] = {
        "ClaudeOS 1.0 - a hobby operating system for i386",
        "",
        "Kernel: C + NASM, Multiboot (GRUB), paging,",
        "preemptive multitasking, ring-3 ELF programs.",
        "Drivers: PS/2 keyboard & mouse, PIT, RTC, ATA,",
        "VESA framebuffer, ACPI power-off.",
        "File system: FAT12/FAT16 (read/write).",
        "",
        "Font: Terminus (SIL Open Font License).",
    };
    gfx_fill(cx + 10, cy + 10, 48, 48, 0x0A246A);
    gfx_fill(cx + 16, cy + 16, 16, 16, 0xE04040); gfx_fill(cx + 36, cy + 16, 16, 16, 0x40C040);
    gfx_fill(cx + 16, cy + 36, 16, 16, 0x4060E0); gfx_fill(cx + 36, cy + 36, 16, 16, 0xE0C040);
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        gfx_text(cx + 70, cy + 10 + i * 18, lines[i], i == 0 ? 0x0A246A : 0);
}

void app_about(void) {
    window_t *w = gui_window_create("About ClaudeOS", gfx_width / 2 - 230, gfx_height / 2 - 140, 460, 220);
    if (!w) return;
    w->paint = about_paint;
    gui_add_widget(w, W_BUTTON, 350, 182, 96, 28, "OK")->on_click = about_ok;
}

/* ===== File browser ========================================================= */
#define FB_MAX 64
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

static void files_mouse(window_t *w, int x, int y) {
    files_t *f = w->data;
    if (x < 4 || x > 224 || y < 34) return;
    int i = (y - 36) / 18;
    if (i < 0 || i >= f->n) return;
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
    gfx_fill(cx + 4, cy + 34, 220, 300, 0xFFFFFF);
    gfx_bevel(cx + 4, cy + 34, 220, 300, false);
    for (int i = 0; i < f->n && i < 16; i++) {
        int y = cy + 36 + i * 18;
        if (i == f->sel) gfx_fill(cx + 6, y, 216, 18, 0x0A246A);
        uint32_t fg = i == f->sel ? 0xFFFFFF : 0;
        if (f->ent[i].is_dir) { gfx_fill(cx + 10, y + 4, 12, 10, 0xE0B040); gfx_fill(cx + 10, y + 2, 6, 3, 0xE0B040); }
        else { gfx_fill(cx + 11, y + 2, 10, 13, 0xFFFFFF); gfx_rect(cx + 11, y + 2, 10, 13, 0x606060); }
        gfx_text(cx + 28, y + 1, f->ent[i].name, fg);
        if (!f->ent[i].is_dir) {
            char sz[16];
            snprintf(sz, sizeof(sz), "%u", f->ent[i].size);
            gfx_text(cx + 216 - (int)strlen(sz) * FONT_W, y + 1, sz, i == f->sel ? 0xC0C0FF : 0x808080);
        }
    }
    int px = cx + 232, py = cy + 34, pw = 340, ph = 300;      /* preview pane */
    gfx_fill(px, py, pw, ph, 0xFFFFF0);
    gfx_bevel(px, py, pw, ph, false);
    int col = 0, row = 0, cols = (pw - 8) / FONT_W, rows = (ph - 4) / FONT_H;
    for (char *p = f->preview; *p && row < rows; p++) {
        if (*p == '\n' || col >= cols) { row++; col = 0; if (*p == '\n') continue; if (row >= rows) break; }
        if (*p == '\t' || *p == '\r') { col += *p == '\t'; continue; }
        gfx_char(px + 4 + col * FONT_W, py + 2 + row * FONT_H, *p, 0x202020, 0, true);
        col++;
    }
    if (!f->preview[0]) gfx_text(px + 10, py + 10, "Click a file to preview it.", 0x808080);
    gfx_text(cx + 6, cy + 340, f->status, 0x404040);
}

void app_files(void) {
    int x, y;
    next_pos(&x, &y);
    window_t *w = gui_window_create("File Browser", x, y, 580, 360);
    if (!w) return;
    files_t *f = kzalloc(sizeof(files_t));
    if (!f) { gui_window_close(w); return; }
    w->data = f;
    w->free_data = true;
    w->paint = files_paint;
    w->on_mouse = files_mouse;
    strlcpy(f->cwd, "/", sizeof(f->cwd));
    gui_add_widget(w, W_BUTTON, 4, 4, 50, 24, "Up")->on_click = files_up;
    gui_add_widget(w, W_BUTTON, 58, 4, 76, 24, "Refresh")->on_click = files_refresh;
    f->path = gui_add_widget(w, W_LABEL, 144, 4, 420, 24, "/");
    f->path->color = 0x0A246A;
    files_load(f);
}
