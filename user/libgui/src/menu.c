/* libgui menus: popup menus and the menu bar widget */
#include <gui.h>
#include <stdlib.h>
#include <string.h>

#define MI_SEP 1
#define MI_CHECK 2
#define MI_CHECKED 4
#define MI_DISABLED 8

typedef struct {
    char text[48];
    char shortcut[24];
    int id;
    int flags;
} menu_item_t;

struct ui_menu {
    menu_item_t items[32];
    int count;
};

ui_menu_t *ui_menu_new(void) { return calloc(1, sizeof(ui_menu_t)); }

static menu_item_t *add_item(ui_menu_t *m) {
    if (m->count >= 32) return 0;
    menu_item_t *it = &m->items[m->count++];
    memset(it, 0, sizeof(*it));
    return it;
}

void ui_menu_item(ui_menu_t *m, const char *text, const char *shortcut, int id) {
    menu_item_t *it = add_item(m);
    if (!it) return;
    strlcpy(it->text, text, sizeof(it->text));
    if (shortcut) strlcpy(it->shortcut, shortcut, sizeof(it->shortcut));
    it->id = id;
}

void ui_menu_check(ui_menu_t *m, const char *text, bool checked, int id) {
    ui_menu_item(m, text, 0, id);
    if (m->count) m->items[m->count - 1].flags |= MI_CHECK | (checked ? MI_CHECKED : 0);
}

void ui_menu_separator(ui_menu_t *m) {
    menu_item_t *it = add_item(m);
    if (it) it->flags = MI_SEP;
}

void ui_menu_enable(ui_menu_t *m, int id, bool en) {
    for (int i = 0; i < m->count; i++)
        if (m->items[i].id == id) {
            if (en) m->items[i].flags &= ~MI_DISABLED;
            else m->items[i].flags |= MI_DISABLED;
        }
}

void ui_menu_set_checked(ui_menu_t *m, int id, bool c) {
    for (int i = 0; i < m->count; i++)
        if (m->items[i].id == id) {
            if (c) m->items[i].flags |= MI_CHECKED;
            else m->items[i].flags &= ~MI_CHECKED;
        }
}

/* ------------------------------------------------------------------ popup */
#define ROW_H 30
#define SEP_H 9

typedef struct {
    ui_menu_t *menu;
    ui_menu_cb cb;
    ui_window_t *owner;
    int hover;
    ui_widget_t *bar;       /* menubar this popup belongs to (or NULL) */
    int bar_index;
} popup_t;

static ui_window_t *open_popup;

static int item_y(ui_menu_t *m, int i) {
    int y = 4;
    for (int k = 0; k < i; k++) y += (m->items[k].flags & MI_SEP) ? SEP_H : ROW_H;
    return y;
}

static int item_at(ui_menu_t *m, int y) {
    int yy = 4;
    for (int i = 0; i < m->count; i++) {
        int h = (m->items[i].flags & MI_SEP) ? SEP_H : ROW_H;
        if (y >= yy && y < yy + h) return (m->items[i].flags & (MI_SEP | MI_DISABLED)) ? -1 : i;
        yy += h;
    }
    return -1;
}

static void popup_paint(ui_window_t *win, surface_t *s) {
    popup_t *p = win->user;
    ui_menu_t *m = p->menu;
    if (ui_is_retro()) {
        gfx_fill(s, 0, 0, win->w, win->h, ui_theme.menu_bg);
        ui_bevel(s, mkrect(0, 0, win->w, win->h), false);
    } else {
        gfx_fill(s, 0, 0, win->w, win->h, ui_theme.menu_bg);
    }
    for (int i = 0; i < m->count; i++) {
        menu_item_t *it = &m->items[i];
        int y = item_y(m, i);
        if (it->flags & MI_SEP) {
            gfx_hline(s, 8, y + 4, win->w - 16, ui_theme.menu_border);
            continue;
        }
        bool hov = i == p->hover;
        uint32_t tc = (it->flags & MI_DISABLED) ? ui_theme.window_text_dim : ui_theme.menu_text;
        if (hov) {
            if (ui_is_retro()) {
                gfx_fill(s, 3, y, win->w - 6, ROW_H, ui_theme.menu_hover);
                tc = 0xFFFFFFFF;
            } else {
                gfx_fill_rounded(s, 4, y, win->w - 8, ROW_H, 5, ui_theme.menu_hover);
            }
        }
        if ((it->flags & MI_CHECK) && (it->flags & MI_CHECKED)) ui_draw_glyph(s, "check", 16, 10, y + 7, tc);
        font_draw(s, &ui_font, 34, y + (ROW_H - ui_font.height) / 2, it->text, tc);
        if (it->shortcut[0]) {
            int sw = font_text_width(&ui_font, it->shortcut);
            font_draw(s, &ui_font, win->w - sw - 14, y + (ROW_H - ui_font.height) / 2, it->shortcut,
                      hov && ui_is_retro() ? tc : ui_theme.window_text_dim);
        }
    }
}

static void popup_select(ui_window_t *win, int i) {
    popup_t *p = win->user;
    ui_menu_t *m = p->menu;
    int id = m->items[i].id;
    ui_menu_cb cb = p->cb;
    ui_window_t *owner = p->owner;
    ui_widget_t *bar = p->bar;
    if (bar) { bar->ival = -1; ui_widget_invalidate(bar); }
    open_popup = 0;
    ui_window_close(win);
    if (cb) cb(owner, id);
}

static void open_bar_menu(ui_widget_t *bar, int index);
static void popup_close_cleanup(ui_window_t *win);

static void popup_event(ui_window_t *win, gui_event_t *ev) {
    popup_t *p = win->user;
    ui_menu_t *m = p->menu;
    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int h = item_at(m, ev->y);
        if (ev->x < 0 || ev->x >= win->w) h = -1;
        if (h != p->hover) { p->hover = h; ui_invalidate(win); }
        break;
    }
    case EV_MOUSE_LEAVE:
        if (p->hover != -1) { p->hover = -1; ui_invalidate(win); }
        break;
    case EV_MOUSE_UP: {
        int h = item_at(m, ev->y);
        if (h >= 0 && ev->x >= 0 && ev->x < win->w) popup_select(win, h);
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESC) {
            popup_close_cleanup(win);
            ui_window_close(win);
        } else if (ev->key == KEY_DOWN || ev->key == KEY_UP) {
            int d = ev->key == KEY_DOWN ? 1 : -1;
            int h = p->hover;
            for (int i = 0; i < m->count; i++) {
                h = (h + d + m->count) % m->count;
                if (!(m->items[h].flags & (MI_SEP | MI_DISABLED))) break;
            }
            p->hover = h;
            ui_invalidate(win);
        } else if ((ev->key == KEY_ENTER || ev->key == KEY_KPENTER) && p->hover >= 0) {
            popup_select(win, p->hover);
        } else if ((ev->key == KEY_LEFT || ev->key == KEY_RIGHT) && p->bar) {
            ui_widget_t *bar = p->bar;
            int n = bar->imax;
            int next = (p->bar_index + (ev->key == KEY_RIGHT ? 1 : n - 1)) % n;
            ui_window_close(win);
            open_popup = 0;
            open_bar_menu(bar, next);
        }
        break;
    case EV_POPUP_CLOSE:
        popup_close_cleanup(win);
        break;
    }
}

static void popup_close_cleanup(ui_window_t *win) {
    popup_t *p = win->user;
    if (p && p->bar) { p->bar->ival = -1; ui_widget_invalidate(p->bar); }
    if (open_popup == win) open_popup = 0;
}

static ui_window_t *popup_create(ui_window_t *owner, ui_menu_t *m, int sx, int sy, ui_menu_cb cb) {
    int w = 160;
    for (int i = 0; i < m->count; i++) {
        int tw = font_text_width(&ui_font, m->items[i].text) + 34 + 20;
        if (m->items[i].shortcut[0]) tw += font_text_width(&ui_font, m->items[i].shortcut) + 30;
        if (tw > w) w = tw;
    }
    int h = item_y(m, m->count) + 4;
    gui_wincreate_t req;
    memset(&req, 0, sizeof(req));
    req.x = sx;
    req.y = sy;
    req.w = w;
    req.h = h;
    req.flags = WF_POPUP | WF_NO_DECOR | WF_NO_TASKBAR;
    req.parent = owner->id;
    strlcpy(req.title, "menu", sizeof(req.title));
    if (open_popup) { popup_close_cleanup(open_popup); ui_window_close(open_popup); }
    popup_t *p = calloc(1, sizeof(popup_t));
    p->menu = m;
    p->cb = cb;
    p->owner = owner;
    p->hover = -1;
    /* the window is created hidden, painted, then shown */
    req.flags |= WF_HIDDEN;
    ui_window_t *win = ui_window_ex(&req);
    if (!win) { free(p); return 0; }
    win->user = p;
    win->on_paint = popup_paint;
    win->on_event = popup_event;
    win->bg = ui_theme.menu_bg;
    win->owner = owner;
    ui_invalidate(win);
    ui_flush(win);
    gui_win_action(win->id, WA_SHOW);
    open_popup = win;
    return win;
}

void ui_menu_popup(ui_window_t *win, ui_menu_t *m, int x, int y, ui_menu_cb cb) {
    int r[8];
    if (gui_win_get_rect(win->id, r) < 0) return;
    popup_create(win, m, r[4] + x, r[5] + y, cb);
}

/* ------------------------------------------------------------------ menubar */
typedef struct {
    char titles[8][24];
    ui_menu_t *menus[8];
    rect_t rects[8];
    ui_menu_cb cb;
    int hover;
} menubar_t;

ui_widget_t *ui_menubar(ui_window_t *win, ui_menu_cb cb) {
    ui_widget_t *w = ui_add(win, W_MENUBAR, 0, 0, win->w, 28, "");
    menubar_t *mb = calloc(1, sizeof(menubar_t));
    mb->cb = cb;
    mb->hover = -1;
    w->data = mb;
    w->ival = -1;     /* open menu index */
    w->imax = 0;      /* number of menus */
    ui_set_anchor(w, A_LEFT | A_RIGHT | A_TOP);
    return w;
}

void ui_menubar_add(ui_widget_t *bar, const char *title, ui_menu_t *m) {
    menubar_t *mb = bar->data;
    if (bar->imax >= 8) return;
    int i = bar->imax++;
    strlcpy(mb->titles[i], title, sizeof(mb->titles[i]));
    mb->menus[i] = m;
    int x = 4;
    for (int k = 0; k < i; k++) x += mb->rects[k].w;
    mb->rects[i] = mkrect(x, 2, font_text_width(&ui_font, title) + 20, bar->r.h - 4);
}

void ui_menubar_draw(ui_widget_t *w, surface_t *s) {
    menubar_t *mb = w->data;
    gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, ui_is_retro() ? ui_theme.window_bg : ui_theme.title_bg);
    gfx_hline(s, w->r.x, w->r.y + w->r.h - 1, w->r.w, ui_theme.input_border);
    for (int i = 0; i < w->imax; i++) {
        rect_t r = mb->rects[i];
        r.x += w->r.x;
        r.y += w->r.y;
        bool open = w->ival == i, hov = mb->hover == i;
        uint32_t tc = ui_theme.window_text;
        if (open || hov) {
            if (ui_is_retro()) {
                if (open) { gfx_fill(s, r.x, r.y, r.w, r.h, ui_theme.menu_hover); tc = 0xFFFFFFFF; }
                else ui_bevel(s, r, false);
            } else {
                gfx_fill_rounded(s, r.x, r.y, r.w, r.h, 5, open ? ui_theme.button_pressed : ui_theme.button_hover);
            }
        }
        ui_draw_text_center(s, &ui_font, r, mb->titles[i], tc);
    }
}

static void bar_menu_cb(ui_window_t *win, int id);
static ui_widget_t *active_bar;

static void open_bar_menu(ui_widget_t *bar, int index) {
    menubar_t *mb = bar->data;
    int r[8];
    if (gui_win_get_rect(bar->win->id, r) < 0) return;
    rect_t tr = mb->rects[index];
    bar->ival = index;
    ui_widget_invalidate(bar);
    active_bar = bar;
    ui_window_t *pw = popup_create(bar->win, mb->menus[index], r[4] + bar->r.x + tr.x, r[5] + bar->r.y + bar->r.h,
                                   bar_menu_cb);
    if (pw) {
        popup_t *p = pw->user;
        p->bar = bar;
        p->bar_index = index;
    }
}

static void bar_menu_cb(ui_window_t *win, int id) {
    if (!active_bar) return;
    menubar_t *mb = active_bar->data;
    if (mb->cb) mb->cb(win, id);
}

static int bar_index_at(ui_widget_t *w, int x, int y) {
    menubar_t *mb = w->data;
    for (int i = 0; i < w->imax; i++) {
        rect_t r = mb->rects[i];
        r.x += w->r.x;
        r.y += w->r.y;
        if (rect_contains(r, x, y)) return i;
    }
    return -1;
}

bool ui_menubar_event(ui_widget_t *w, gui_event_t *ev) {
    menubar_t *mb = w->data;
    if (ev->type == EV_MOUSE_MOVE) {
        int h = bar_index_at(w, ev->x, ev->y);
        if (h != mb->hover) { mb->hover = h; ui_widget_invalidate(w); }
        /* slide between menus while one is open */
        if (h >= 0 && w->ival >= 0 && h != w->ival && open_popup) {
            ui_window_close(open_popup);
            open_popup = 0;
            open_bar_menu(w, h);
        }
        return true;
    }
    if (ev->type == EV_MOUSE_LEAVE) {
        if (mb->hover != -1) { mb->hover = -1; ui_widget_invalidate(w); }
        return true;
    }
    if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) {
        int h = bar_index_at(w, ev->x, ev->y);
        if (h >= 0) open_bar_menu(w, h);
        return true;
    }
    return ev->type != EV_KEY_DOWN;
}
