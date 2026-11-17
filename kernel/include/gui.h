/* Window manager, widget toolkit, themes and animations. Runs as one kernel
 * task (gui_task); see gui/gui.c. */
#ifndef GUI_H
#define GUI_H
#include "types.h"
#include "tty.h"
#include "gfx.h"

#define MAX_WINDOWS  16
#define MAX_WIDGETS  40
#define TITLE_H      36
#define TASKBAR_H    48
#define WIN_RADIUS   10

/* Widget types and styles. */
enum { W_LABEL, W_BUTTON, W_TEXTBOX, W_TOGGLE, W_SLIDER, W_CHOICE, W_SWATCH };
enum { S_NORMAL, S_DIM, S_ACCENT, S_PRIMARY, S_HEADING, S_DANGER, S_BIG, S_WALLPAPER };

/* Applications (desktop icons, launcher, taskbar icons). */
enum { APP_TERMINAL, APP_FILES, APP_NOTES, APP_CALC, APP_MONITOR, APP_WIDGETS, APP_SNAKE,
       APP_SETTINGS, APP_ABOUT, APP_COUNT };

/* Mouse events delivered to window->on_mouse. */
enum { MOUSE_DOWN, MOUSE_DRAG, MOUSE_UP };

struct window;
typedef struct widget {
    int  type, style;
    int  x, y, w, h;            /* relative to the window client area */
    char text[96];              /* label/button text; W_CHOICE: "A|B|C" */
    char placeholder[40];       /* W_TEXTBOX hint shown while empty */
    int  cursor;                /* W_TEXTBOX caret */
    bool sel_on;                /* W_TEXTBOX selection between sel and cursor */
    int  sel;
    bool pressed;
    int  value, min, max;       /* toggle 0/1, slider range, choice index, swatch selected */
    int  anim;                  /* animated state 0..1000 (toggle knob, choice highlight) */
    uint32_t color;             /* swatch colour / custom label colour (0 = theme) */
    void (*on_click)(struct window *w, struct widget *wd);    /* button, swatch, slider release */
    void (*on_enter)(struct window *w, struct widget *wd);    /* Enter in a text box */
    void (*on_change)(struct window *w, struct widget *wd);   /* toggle, slider, choice */
    int  tag;
} widget_t;

typedef struct window {
    bool used, minimized, closing;
    int  x, y, w, h;            /* outer frame, title bar included */
    char title[48];             /* may contain %d, filled from title_num */
    int  title_num;
    int  app;                   /* APP_* for the icon */
    widget_t *widgets[MAX_WIDGETS];
    int  nwidgets;
    int  focus;                 /* focused widget index or -1 */
    surface_t *surf;            /* off-screen image of the window */
    int  anim;                  /* running animation (ANIM_*) */
    uint32_t anim_start;
    void (*paint)(struct window *w, int cx, int cy);   /* custom client drawing */
    void (*on_key)(struct window *w, int key);
    void (*on_close)(struct window *w);
    void (*on_tick)(struct window *w);                 /* every GUI loop (~15 ms) */
    void (*on_mouse)(struct window *w, int x, int y, int ev);  /* MOUSE_DOWN/DRAG/UP in client */
    void (*on_context)(struct window *w, int x, int y);        /* right click in client */
    void *data;
    bool free_data;             /* kfree(data) when the window closes */
} window_t;

typedef struct {
    uint32_t window, title, text, dim, border, button, button_hover, button_press;
    uint32_t input, input_border, panel, menu, hover, card;
} theme_t;

/* User settings (Settings app, saved in /etc/gui.cfg). */
typedef struct {
    int  theme;                 /* 0 dark, 1 light */
    int  accent;                /* index into accent_colors */
    int  wallpaper;             /* index into wallpaper presets */
    bool animations;
    int  anim_speed;            /* 0 slow, 1 normal, 2 fast */
    bool shadows;
    bool translucent;           /* see-through taskbar and menus */
    bool seconds;               /* clock shows seconds */
    int  mouse_speed;           /* percent */
    int  lang;                  /* 0 English, 1 Russian */
} gui_settings_t;

#define NUM_ACCENTS    6
#define NUM_WALLPAPERS 5
extern gui_settings_t gui_settings;
extern const uint32_t accent_colors[NUM_ACCENTS];
extern const char *const accent_names[NUM_ACCENTS];
extern const char *const wallpaper_names[NUM_WALLPAPERS];

/* Context menus: label NULL = separator. Shown at the mouse position. */
typedef struct {
    const char *label;
    void (*fn)(struct window *w, struct widget *wd);
    bool disabled;
} gui_menu_item_t;
#define GUI_MENU_MAX 10
void gui_show_menu(const gui_menu_item_t *items, int n, window_t *w, widget_t *wd);

/* Translation of UI strings (English source text -> current language). */
const char *T(const char *en);

const theme_t *gui_theme(void);
uint32_t gui_accent(void);
void gui_settings_defaults(void);
void gui_settings_changed(bool save);   /* apply (and optionally save) settings */
void gui_request_settings_apply(void);  /* same, from another task (shell) */
void gui_wallpaper_colors(int i, uint32_t *c1, uint32_t *c2);

void      gui_task(void *arg);          /* display task: CLI redraws + GUI loop */
void      gui_post_key(int key);
void      gui_invalidate(void);
void      gui_open_app(int app);
void      gui_request_app(int app);     /* open an app from another task */
void      gui_toast(const char *msg);   /* small animated notification */
window_t *gui_window_create(int app, const char *title, int x, int y, int cw, int ch);
void      gui_window_close(window_t *w);
widget_t *gui_add_widget(window_t *w, int type, int x, int y, int ww, int wh, const char *text);
void      gui_client_size(window_t *w, int *cw, int *ch);
bool      gui_is_focused(window_t *w);
bool      gui_blink_on(void);           /* caret blink phase */
bool      gui_mouse_in_client(window_t *w, int *x, int *y);
void      gui_draw_app_icon(int app, int x, int y, int size);
void      gui_draw_logo(int cx, int cy, int r, uint32_t color);

/* Built-in applications (gui/apps.c). */
void app_terminal(void);
void app_files(void);
void app_notes(void);
void app_notes_open(const char *path);
void app_snake(void);
void app_calculator(void);
void app_sysmon(void);
void app_widgets_demo(void);
void app_settings(void);
void app_about(void);
#endif
