#pragma once
/* libgui: ClaudeOS user interface toolkit */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <claudeos/gfx.h>
#include <claudeos/font.h>
#include <claudeos/gui.h>
#include <claudeos/keys.h>
#include <claudeos/image.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define MIN_W_DEFAULT(w) ((w) < 240 ? (w) : 240)

/* ------------------------------------------------------------------ raw system interface */
int gui_win_create(const gui_wincreate_t *req);
int gui_win_destroy(int id);
int gui_win_update(int id, const uint32_t *px, int stride, int x, int y, int w, int h);
int gui_win_set_title(int id, const char *title);
int gui_win_resize(int id, int w, int h);
int gui_win_move(int id, int x, int y);
int gui_win_action(int id, int action);
int gui_win_get_rect(int id, int out[8]);   /* frame x,y,w,h, content x,y,w,h (screen) */
int gui_win_set_cursor(int id, int shape);
int gui_get_event(gui_event_t *ev, int timeout_ms);
int gui_screen_info(gui_screen_t *out);
int gui_theme_get(gui_theme_t *out, int preset_index);    /* 0 = current, n = preset n-1 */
int gui_config_get(gui_config_t *out);
int gui_config_set(const gui_config_t *c, int preview);
int gui_clipboard_set(const char *text, size_t len);
long gui_clipboard_get(char *buf, size_t max);             /* returns full length */
int gui_notify(const char *title, const char *text, const char *icon);
int gui_set_resolution(int w, int h);
int gui_launch(const char *path, const char *arg);         /* arg NULL: open file with default app */

/* ------------------------------------------------------------------ toolkit globals */
extern gui_theme_t ui_theme;
extern font_t ui_font, ui_font_bold, ui_font_large, ui_font_title, ui_font_mono, ui_font_mono_bold,
              ui_font_display, ui_font_big;

bool ui_init(void);
void ui_reload_theme(void);

typedef struct { int w, h; const uint32_t *px; } ui_image_t;
ui_image_t *ui_icon(const char *name, int size);          /* colour icon */
ui_image_t *ui_glyph(const char *name, int size);         /* white glyph */
void ui_draw_icon(surface_t *s, const char *name, int size, int x, int y);
void ui_draw_glyph(surface_t *s, const char *name, int size, int x, int y, uint32_t color);

/* ------------------------------------------------------------------ widgets & windows */
typedef struct ui_window ui_window_t;
typedef struct ui_widget ui_widget_t;

enum {
    W_LABEL = 1, W_BUTTON, W_TEXTBOX, W_CHECKBOX, W_SLIDER, W_LIST, W_PROGRESS, W_DROPDOWN, W_CANVAS,
    W_PANEL, W_IMAGE, W_MENUBAR, W_SEPARATOR, W_TOOLBUTTON, W_SWATCH
};

/* anchors: which window edges the widget sticks to when the window is resized */
#define A_LEFT   1
#define A_RIGHT  2
#define A_TOP    4
#define A_BOTTOM 8
#define A_ALL    15

typedef void (*ui_cb)(ui_widget_t *w);

struct ui_widget {
    int type;
    rect_t r;
    int anchor;
    int mr, mb;                 /* distance to right/bottom edge at creation */
    bool visible, enabled, focusable;
    char *text;
    int id;
    uint32_t color;             /* type specific (label colour, swatch colour, ...) */
    const font_t *font;
    int align;                  /* 0 left, 1 center, 2 right */
    ui_cb on_click, on_change, on_activate;
    void (*draw)(ui_widget_t *w, surface_t *s);            /* canvas */
    bool (*event)(ui_widget_t *w, gui_event_t *ev);        /* canvas / internal */
    void *data;                 /* type specific state */
    void *user;
    int ival;                   /* checkbox state, slider value, list selection ... */
    int imin, imax;
    char glyph[24];
    int cursor;                 /* mouse cursor shape over this widget (0 = default) */
    ui_window_t *win;
    ui_widget_t *next;
};

struct ui_window {
    int id;
    int w, h;
    surface_t surf;
    uint32_t *px;
    ui_widget_t *widgets;
    ui_widget_t *focus, *hover, *capture;
    rect_t dirty;
    bool alive, focused, is_popup;
    uint32_t flags;
    uint32_t bg;                /* 0 = theme window_bg */
    void (*on_paint)(ui_window_t *w, surface_t *s);            /* before widgets */
    void (*on_event)(ui_window_t *w, gui_event_t *ev);         /* raw events (after widgets) */
    void (*on_resize)(ui_window_t *w);
    bool (*on_close)(ui_window_t *w);                          /* return false to veto */
    void (*on_key)(ui_window_t *w, gui_event_t *ev);           /* keys not used by a widget */
    void *user;
    ui_window_t *next;
    ui_window_t *owner;         /* for popups */
};

ui_window_t *ui_window(const char *title, int w, int h, uint32_t flags, const char *icon);
ui_window_t *ui_window_ex(const gui_wincreate_t *req);
void ui_window_close(ui_window_t *win);
void ui_set_title(ui_window_t *win, const char *title);
void ui_invalidate(ui_window_t *win);
void ui_invalidate_rect(ui_window_t *win, rect_t r);
void ui_widget_invalidate(ui_widget_t *w);
void ui_flush(ui_window_t *win);
void ui_resize(ui_window_t *win, int w, int h);
void ui_set_cursor(ui_window_t *win, int shape);
void ui_focus(ui_window_t *win, ui_widget_t *w);

ui_widget_t *ui_add(ui_window_t *win, int type, int x, int y, int w, int h, const char *text);
ui_widget_t *ui_label(ui_window_t *win, int x, int y, int w, int h, const char *text);
ui_widget_t *ui_button(ui_window_t *win, int x, int y, int w, int h, const char *text, ui_cb cb);
ui_widget_t *ui_toolbutton(ui_window_t *win, int x, int y, int size, const char *glyph, const char *tip, ui_cb cb);
ui_widget_t *ui_textbox(ui_window_t *win, int x, int y, int w, int h, const char *text);
ui_widget_t *ui_checkbox(ui_window_t *win, int x, int y, int w, int h, const char *text, bool checked);
ui_widget_t *ui_slider(ui_window_t *win, int x, int y, int w, int h, int min, int max, int value);
ui_widget_t *ui_list(ui_window_t *win, int x, int y, int w, int h);
ui_widget_t *ui_progress(ui_window_t *win, int x, int y, int w, int h);
ui_widget_t *ui_dropdown(ui_window_t *win, int x, int y, int w, int h);
ui_widget_t *ui_canvas(ui_window_t *win, int x, int y, int w, int h,
                       void (*draw)(ui_widget_t *, surface_t *), bool (*event)(ui_widget_t *, gui_event_t *));
ui_widget_t *ui_panel(ui_window_t *win, int x, int y, int w, int h, uint32_t color);
ui_widget_t *ui_swatch(ui_window_t *win, int x, int y, int w, int h, uint32_t color, ui_cb cb);
void ui_set_text(ui_widget_t *w, const char *text);
const char *ui_get_text(ui_widget_t *w);
void ui_set_anchor(ui_widget_t *w, int anchor);
void ui_show(ui_widget_t *w, bool visible);
void ui_enable(ui_widget_t *w, bool enabled);

/* button styles */
#define BTN_PRIMARY 1
void ui_button_style(ui_widget_t *w, int style);

/* textbox */
void ui_textbox_select_all(ui_widget_t *w);
void ui_textbox_set_placeholder(ui_widget_t *w, const char *text);
void ui_textbox_set_password(ui_widget_t *w, bool pw);

/* list: rows with optional icon and tab separated columns */
typedef struct { const char *title; int width; } ui_column_t;
void ui_list_clear(ui_widget_t *w);
int ui_list_add(ui_widget_t *w, const char *text, const char *icon, void *data);
void ui_list_set_columns(ui_widget_t *w, const ui_column_t *cols, int n);
int ui_list_count(ui_widget_t *w);
const char *ui_list_text(ui_widget_t *w, int i);
void *ui_list_data(ui_widget_t *w, int i);
void ui_list_select(ui_widget_t *w, int i);
void ui_list_set_row_height(ui_widget_t *w, int h);
void ui_list_set_icon_mode(ui_widget_t *w, bool grid);       /* large icon grid view */
void ui_list_set_text(ui_widget_t *w, int i, const char *text);

/* dropdown */
void ui_dropdown_add(ui_widget_t *w, const char *text);
void ui_dropdown_clear(ui_widget_t *w);
void ui_dropdown_select(ui_widget_t *w, int i);

/* ------------------------------------------------------------------ menus */
typedef struct ui_menu ui_menu_t;
typedef void (*ui_menu_cb)(ui_window_t *win, int id);

ui_menu_t *ui_menu_new(void);
void ui_menu_item(ui_menu_t *m, const char *text, const char *shortcut, int id);
void ui_menu_check(ui_menu_t *m, const char *text, bool checked, int id);
void ui_menu_separator(ui_menu_t *m);
void ui_menu_enable(ui_menu_t *m, int id, bool enabled);
void ui_menu_set_checked(ui_menu_t *m, int id, bool checked);
void ui_menu_popup(ui_window_t *win, ui_menu_t *m, int x, int y, ui_menu_cb cb);   /* window-relative */
ui_widget_t *ui_menubar(ui_window_t *win, ui_menu_cb cb);
void ui_menubar_add(ui_widget_t *bar, const char *title, ui_menu_t *m);

/* ------------------------------------------------------------------ dialogs (modal) */
int ui_msgbox(ui_window_t *parent, const char *title, const char *text, const char *buttons);  /* "OK|Cancel" */
char *ui_input(ui_window_t *parent, const char *title, const char *prompt, const char *initial);  /* malloc'd or NULL */
char *ui_file_dialog(ui_window_t *parent, bool save, const char *title, const char *dir, const char *name,
                     const char *filter);   /* filter: "txt;md" or NULL */
bool ui_color_dialog(ui_window_t *parent, uint32_t *color);

/* ------------------------------------------------------------------ main loop */
void ui_run(void);                       /* until all windows are closed or ui_quit() */
void ui_quit(void);
bool ui_step(int timeout_ms);            /* process events once; false when quitting */
int ui_timer(int interval_ms, void (*cb)(void *), void *arg);   /* repeating timer, returns id */
void ui_timer_cancel(int id);
void ui_watch_fd(int fd, void (*cb)(int fd, void *), void *arg);
void ui_unwatch_fd(int fd);

/* ------------------------------------------------------------------ drawing helpers */
void ui_draw_button_frame(surface_t *s, rect_t r, int state, bool primary);   /* state: 0 normal 1 hover 2 pressed 3 disabled */
void ui_draw_input_frame(surface_t *s, rect_t r, bool focused);
void ui_draw_scrollbar(surface_t *s, rect_t track, int total, int visible, int pos, bool hover);
void ui_draw_text_center(surface_t *s, const font_t *f, rect_t r, const char *text, uint32_t color);
uint32_t ui_bg(void);
bool ui_is_retro(void);
void ui_bevel(surface_t *s, rect_t r, bool sunken);

/* images: decode PNG/JPEG/GIF/BMP (malloc'd 0xAARRGGBB) and resize */
uint32_t *ui_decode_image(const void *data, size_t len, int *w, int *h);
uint32_t *ui_scale_image(const uint32_t *src, int sw, int sh, int dw, int dh);

/* misc */
const char *ui_basename(const char *path);
char *ui_read_file(const char *path, size_t *size);      /* malloc'd, NUL terminated */
bool ui_write_file(const char *path, const void *data, size_t size);
