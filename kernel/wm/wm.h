#pragma once
/* ClaudeOS window server (kernel side) - internal interfaces */
#include <kernel.h>
#include <sched.h>
#include <fb.h>
#include <input.h>
#include <claudeos/gfx.h>
#include <claudeos/font.h>
#include <claudeos/gui.h>

typedef struct icon {
    int w, h;
    const uint32_t *px;
    void *data;
} icon_t;

icon_t *icon_get(const char *name, int size);      /* colour icon, NULL if missing */
icon_t *glyph_get(const char *name, int size);     /* white glyph for tinting */
icon_t *cursor_icon(int shape, int *hx, int *hy);
void draw_icon(surface_t *s, icon_t *ic, int x, int y);
void draw_glyph(surface_t *s, const char *name, int size, int x, int y, uint32_t color);

#define EVQ_SIZE 256

typedef struct gui_client {
    task_t *task;
    int pid;
    gui_event_t q[EVQ_SIZE];
    int head, tail;
    waitq_t wq;
    int nwindows;
} gui_client_t;

typedef struct window {
    int id;
    gui_client_t *client;
    char title[64];
    char icon[32];
    uint32_t flags;
    int x, y;               /* frame origin */
    int cw, ch;             /* content size */
    uint32_t *buf;
    size_t buf_bytes;
    bool visible, minimized, maximized;
    int snapped;            /* 0 none, 1 left half, 2 right half */
    rect_t restore;         /* frame rect before maximize/snap */
    int min_w, min_h;
    int parent;
    int cursor;
    uint64_t last_resize_ev;
    struct window *next;    /* z-order: bottom -> top */
} window_t;

/* global state */
typedef struct {
    surface_t scr;              /* back buffer */
    int w, h;
    window_t *windows;          /* bottom of z-order */
    window_t *focused;
    int next_id;
    int mx, my;
    uint32_t buttons;
    int cursor_shape;
    gui_theme_t theme;
    gui_config_t cfg;
    mutex_t lock;
    bool ready;
} wm_state_t;

extern wm_state_t wm;

/* wm.c */
void wm_damage(rect_t r);
void wm_damage_all(void);
rect_t win_frame(window_t *w);
rect_t win_content(window_t *w);
int win_border(void);
int win_title_h(void);
window_t *win_find(int id);
void win_focus(window_t *w);
void win_raise(window_t *w);
void win_minimize(window_t *w);
void win_restore(window_t *w);
void win_toggle_maximize(window_t *w);
void win_close_request(window_t *w);
void wm_send(window_t *w, gui_event_t *ev);
rect_t wm_work_area(void);
void wm_launch(const char *path, const char *arg);
void wm_apply_config(bool rerender_wallpaper);
void wm_reload_theme(void);
void wm_screen_changed(void);
void wm_notify(const char *title, const char *text, const char *icon);

/* theme.c */
void theme_init(void);
bool theme_by_name(const char *name, gui_theme_t *out);
int theme_count(void);
const char *theme_name(int i);
void theme_apply_config(gui_theme_t *t, const gui_config_t *c);
void config_defaults(gui_config_t *c);
void config_load(gui_config_t *c);
void config_save(const gui_config_t *c);

/* wallpaper.c */
void wallpaper_render(void);
void wallpaper_draw(surface_t *s, rect_t r);

/* shell.c: taskbar, start menu, desktop, popups */
void shell_init(void);
int shell_taskbar_height(void);
rect_t shell_taskbar_rect(void);
void shell_draw(surface_t *s, rect_t clip, int layer);   /* layer 0: desktop, 1: taskbar, 2: overlays */
bool shell_mouse(int type, int x, int y, uint32_t button, int clicks, int wheel);  /* true if consumed */
bool shell_key(uint32_t key, uint32_t ch, uint32_t mods, bool pressed);           /* true if consumed */
void shell_tick(void);
bool shell_overlay_open(void);
void shell_close_overlays(void);
void shell_windows_changed(void);
void shell_alt_tab(bool forward);
void shell_alt_release(void);
void shell_desktop_reload(void);

/* fonts */
#define F_UI (&kfont_ui)
#define F_UIB (&kfont_ui_bold)
#define F_LARGE (&kfont_ui_large)
#define F_TITLE (&kfont_title)
#define F_DISPLAY (&kfont_display)
