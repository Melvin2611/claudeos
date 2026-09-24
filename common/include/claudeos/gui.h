#pragma once
/* Window server ABI shared by the kernel (window manager) and libgui */
#include <stdint.h>

/* window flags */
#define WF_RESIZABLE    0x0001
#define WF_NO_DECOR     0x0002   /* no title bar / border */
#define WF_POPUP        0x0004   /* menu-like: topmost, closed by clicks outside (EV_POPUP_CLOSE) */
#define WF_NO_TASKBAR   0x0008
#define WF_TOPMOST      0x0010
#define WF_CENTER       0x0020   /* center on screen (or on parent) */
#define WF_ALPHA        0x0040   /* content uses per-pixel alpha */
#define WF_MAXIMIZED    0x0080
#define WF_NO_MAXIMIZE  0x0100
#define WF_DIALOG       0x0200   /* no minimize/maximize buttons */
#define WF_HIDDEN       0x0400   /* created invisible */
#define WF_SHADOW       0x0800   /* force a shadow on undecorated windows */

typedef struct {
    int32_t x, y;           /* frame position, -1 = automatic placement */
    int32_t w, h;           /* content size */
    uint32_t flags;
    int32_t min_w, min_h;
    int32_t parent;         /* owning window id (dialogs, popups) or 0 */
    char title[64];
    char icon[32];          /* icon name in /system/icons (e.g. "terminal") */
} gui_wincreate_t;

enum {
    EV_NONE = 0, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_MOUSE_WHEEL, EV_MOUSE_LEAVE,
    EV_KEY_DOWN, EV_KEY_UP, EV_RESIZE, EV_CLOSE, EV_FOCUS, EV_UNFOCUS, EV_THEME, EV_POPUP_CLOSE,
    EV_SCREEN, EV_MINIMIZE, EV_RESTORE
};

typedef struct {
    uint32_t type;
    int32_t win;
    int32_t x, y;           /* mouse position relative to the content area */
    int32_t wheel;          /* + down, - up */
    uint32_t buttons;       /* buttons currently held */
    uint32_t button;        /* button that changed */
    uint32_t clicks;        /* 2 = double click */
    uint32_t key, ch, mods; /* keyboard */
    int32_t w, h;           /* EV_RESIZE / EV_SCREEN: new size */
    int32_t sx, sy;         /* mouse position in screen coordinates */
} gui_event_t;

/* window actions (SYS_WIN_ACTION) */
enum { WA_MINIMIZE = 1, WA_MAXIMIZE, WA_RESTORE, WA_FOCUS, WA_SHOW, WA_HIDE, WA_TOGGLE_MAXIMIZE, WA_RAISE };

/* cursor shapes */
enum { CUR_ARROW = 0, CUR_TEXT, CUR_HAND, CUR_WAIT, CUR_RESIZE_H, CUR_RESIZE_V, CUR_RESIZE_NWSE,
       CUR_RESIZE_NESW, CUR_MOVE, CUR_CROSS, CUR_COUNT };

typedef struct {
    char name[32];
    uint32_t accent, accent_text;
    uint32_t desktop_bg;
    uint32_t window_bg, window_text, window_text_dim;
    uint32_t title_bg, title_bg_inactive, title_text, title_text_inactive;
    uint32_t border, border_inactive;
    uint32_t button_bg, button_hover, button_pressed, button_text, button_border;
    uint32_t input_bg, input_border, input_text;
    uint32_t selection_bg, selection_text;
    uint32_t menu_bg, menu_text, menu_hover, menu_border;
    uint32_t panel_bg, panel_text, panel_hover, panel_active;
    uint32_t list_alt, scroll_track, scroll_thumb;
    uint32_t tooltip_bg, tooltip_text;
    uint32_t sidebar_bg;
    uint8_t dark;           /* dark color scheme */
    uint8_t style;          /* 0 = flat modern, 1 = retro 3D */
    uint8_t radius;         /* window corner radius */
    uint8_t shadows;        /* drop shadows on */
    uint8_t panel_alpha;    /* taskbar opacity 0..255 */
    uint8_t title_h;        /* title bar height */
    uint8_t reserved[2];
} gui_theme_t;

/* wallpaper styles */
enum { WP_GRADIENT = 0, WP_SOLID, WP_WAVES, WP_AURORA, WP_MOUNTAINS, WP_BUBBLES, WP_IMAGE, WP_COUNT };
/* image placement */
enum { WPM_FILL = 0, WPM_FIT, WPM_STRETCH, WPM_CENTER, WPM_TILE };

typedef struct {
    char theme[32];
    uint32_t accent;             /* 0 = use theme accent */
    uint32_t wallpaper;          /* WP_* */
    uint32_t wp_color1, wp_color2;
    char wp_image[128];
    uint32_t wp_mode;            /* WPM_* */
    uint32_t taskbar_top;        /* 0 bottom, 1 top */
    uint32_t taskbar_size;       /* 0 small, 1 normal, 2 large */
    uint32_t taskbar_labels;     /* show window titles on taskbar buttons */
    uint32_t clock_24h, clock_seconds, clock_date;
    uint32_t desktop_icons;
    uint32_t mouse_speed;        /* 1..10 */
    uint32_t double_click_ms;
    char kbd_layout[8];
    uint32_t volume;             /* 0..100 */
    uint32_t sounds;             /* system sounds */
    int32_t radius;              /* -1 = theme default */
    int32_t shadows;             /* -1 = theme default */
    int32_t transparency;        /* translucent taskbar */
    uint32_t reserved[8];
} gui_config_t;

typedef struct {
    int32_t width, height;
    int32_t work_x, work_y, work_w, work_h;   /* screen minus taskbar */
    int32_t can_set_mode;
    int32_t reserved;
} gui_screen_t;
