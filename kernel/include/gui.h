/* Tiny window manager + widget toolkit running as a kernel task. */
#ifndef GUI_H
#define GUI_H
#include "types.h"
#include "tty.h"

#define MAX_WINDOWS  16
#define MAX_WIDGETS  32
#define TITLE_H      22
#define TASKBAR_H    30

enum { W_LABEL, W_BUTTON, W_TEXTBOX };

struct window;
typedef struct widget {
    int  type;
    int  x, y, w, h;            /* relative to the window client area */
    char text[96];
    int  cursor;                /* textbox caret */
    bool pressed;
    uint32_t color;
    void (*on_click)(struct window *w, struct widget *wd);
    void (*on_enter)(struct window *w, struct widget *wd);
    int  tag;
} widget_t;

typedef struct window {
    bool used, minimized;
    int  x, y, w, h;            /* outer frame */
    char title[48];
    widget_t *widgets[MAX_WIDGETS];
    int  nwidgets;
    int  focus;                 /* focused widget index or -1 */
    uint32_t bg;
    void (*paint)(struct window *w, int cx, int cy);   /* custom client drawing */
    void (*on_key)(struct window *w, int key);
    void (*on_close)(struct window *w);
    void (*on_tick)(struct window *w);                 /* called every GUI loop (~15 ms) */
    void (*on_mouse)(struct window *w, int x, int y);  /* click in client area */
    void *data;
    bool free_data;             /* kfree(data) when the window closes */
} window_t;

void      gui_task(void *arg);          /* display task: CLI redraws + GUI loop */
void      gui_post_key(int key);
void      gui_invalidate(void);
window_t *gui_window_create(const char *title, int x, int y, int cw, int ch);
void      gui_window_close(window_t *w);
widget_t *gui_add_widget(window_t *w, int type, int x, int y, int ww, int wh, const char *text);
void      gui_client_size(window_t *w, int *cw, int *ch);

/* Built-in applications (gui/apps.c). */
void app_terminal(void);
void app_calculator(void);
void app_widgets_demo(void);
void app_sysmon(void);
void app_about(void);
void app_files(void);
#endif
