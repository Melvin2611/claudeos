/* libgui core: system calls, fonts, icons, windows, event loop */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <claudeos.h>

gui_theme_t ui_theme;
font_t ui_font, ui_font_bold, ui_font_large, ui_font_title, ui_font_mono, ui_font_mono_bold, ui_font_display, ui_font_big;

static ui_window_t *windows;
static bool quitting;
static ui_window_t *modal;

void ui_set_modal(ui_window_t *w) { modal = w; }
ui_window_t *ui_get_modal(void) { return modal; }

/* while a modal dialog is open, input for other windows is dropped */
static bool blocked_by_modal(ui_window_t *w, gui_event_t *ev) {
    if (!modal || !modal->alive || w == modal || w->owner == modal) return false;
    switch (ev->type) {
    case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_KEY_DOWN: case EV_KEY_UP: case EV_MOUSE_WHEEL: case EV_CLOSE:
        if (ev->type == EV_MOUSE_DOWN || ev->type == EV_CLOSE) gui_win_action(modal->id, WA_FOCUS);
        return true;
    }
    return false;
}

void ui_widget_draw(ui_widget_t *w, surface_t *s);
bool ui_widget_event(ui_widget_t *w, gui_event_t *ev);
void ui_widget_free(ui_widget_t *w);

/* ------------------------------------------------------------------ syscalls */
int gui_win_create(const gui_wincreate_t *req) { return (int)syscall1(SYS_WIN_CREATE, req); }
int gui_win_destroy(int id) { return (int)syscall1(SYS_WIN_DESTROY, id); }
int gui_win_update(int id, const uint32_t *px, int stride, int x, int y, int w, int h) {
    return (int)syscall5(SYS_WIN_UPDATE, id, px, stride, ((uint64_t)(uint32_t)x << 32) | (uint32_t)y,
                         ((uint64_t)(uint32_t)w << 32) | (uint32_t)h);
}
int gui_win_set_title(int id, const char *t) { return (int)syscall2(SYS_WIN_SET_TITLE, id, t); }
int gui_win_resize(int id, int w, int h) { return (int)syscall3(SYS_WIN_RESIZE, id, w, h); }
int gui_win_move(int id, int x, int y) { return (int)syscall3(SYS_WIN_MOVE, id, x, y); }
int gui_win_action(int id, int a) { return (int)syscall2(SYS_WIN_ACTION, id, a); }
int gui_win_get_rect(int id, int out[8]) { return (int)syscall2(SYS_WIN_GET_RECT, id, out); }
int gui_win_set_cursor(int id, int shape) { return (int)syscall2(SYS_WIN_SET_CURSOR, id, shape); }
int gui_get_event(gui_event_t *ev, int timeout) { return (int)syscall2(SYS_GUI_EVENT, ev, timeout); }
int gui_screen_info(gui_screen_t *o) { return (int)syscall1(SYS_SCREEN_INFO, o); }
int gui_theme_get(gui_theme_t *o, int idx) { return (int)syscall2(SYS_THEME_GET, o, idx); }
int gui_config_get(gui_config_t *o) { return (int)syscall1(SYS_WM_CONFIG_GET, o); }
int gui_config_set(const gui_config_t *c, int preview) { return (int)syscall2(SYS_WM_CONFIG_SET, c, preview); }
int gui_clipboard_set(const char *t, size_t n) { return (int)syscall2(SYS_CLIPBOARD_SET, t, n); }
long gui_clipboard_get(char *buf, size_t max) { return syscall2(SYS_CLIPBOARD_GET, buf, max); }
int gui_notify(const char *t, const char *x, const char *i) { return (int)syscall3(SYS_NOTIFY, t, x, i); }
int gui_set_resolution(int w, int h) { return (int)syscall2(SYS_SET_RESOLUTION, w, h); }
int gui_launch(const char *p, const char *a) { return (int)syscall2(SYS_LAUNCH, p, a); }

/* ------------------------------------------------------------------ files */
char *ui_read_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    kstat_t k;
    if (syscall2(SYS_FSTAT, fd, &k) < 0) { close(fd); return 0; }
    size_t sz = k.size;
    char *buf = malloc(sz + 1);
    if (!buf) { close(fd); return 0; }
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    buf[got] = 0;
    if (size) *size = got;
    return buf;
}

bool ui_write_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, (const char *)data + off, size - off);
        if (w <= 0) { close(fd); return false; }
        off += w;
    }
    fsync(fd);
    close(fd);
    return true;
}

const char *ui_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ------------------------------------------------------------------ fonts, theme, icons */
static bool load_font(font_t *f, const char *name) {
    char path[64];
    snprintf(path, sizeof(path), "/system/fonts/%s.fnt", name);
    size_t sz;
    char *data = ui_read_file(path, &sz);
    if (!data) return false;
    return font_load(f, data, sz);
}

void ui_reload_theme(void) { gui_theme_get(&ui_theme, 0); }

bool ui_init(void) {
    static bool done;
    if (done) return true;
    bool ok = load_font(&ui_font, "ui");
    load_font(&ui_font_bold, "ui-bold");
    load_font(&ui_font_large, "ui-large");
    load_font(&ui_font_title, "title");
    load_font(&ui_font_mono, "mono");
    load_font(&ui_font_mono_bold, "mono-bold");
    load_font(&ui_font_display, "display");
    load_font(&ui_font_big, "big-bold");
    ui_reload_theme();
    done = ok;
    if (!ok) fprintf(stderr, "libgui: could not load fonts\n");
    return ok;
}

typedef struct icache { char key[64]; ui_image_t img; bool missing; struct icache *next; } icache_t;
static icache_t *icons;

static ui_image_t *icon_load(const char *path) {
    for (icache_t *c = icons; c; c = c->next)
        if (!strcmp(c->key, path)) return c->missing ? 0 : &c->img;
    icache_t *c = calloc(1, sizeof(icache_t));
    strlcpy(c->key, path, sizeof(c->key));
    size_t sz;
    char *data = ui_read_file(path, &sz);
    const uint32_t *px;
    if (data && icn_parse(data, sz, &c->img.w, &c->img.h, &px)) c->img.px = px;
    else { free(data); c->missing = true; }
    c->next = icons;
    icons = c;
    return c->missing ? 0 : &c->img;
}

ui_image_t *ui_icon(const char *name, int size) {
    char path[64];
    snprintf(path, sizeof(path), "/system/icons/%s-%d.icn", name, size);
    return icon_load(path);
}

ui_image_t *ui_glyph(const char *name, int size) {
    char path[64];
    snprintf(path, sizeof(path), "/system/icons/glyph/%s-%d.icn", name, size);
    return icon_load(path);
}

void ui_draw_icon(surface_t *s, const char *name, int size, int x, int y) {
    ui_image_t *i = ui_icon(name, size);
    if (i) gfx_draw_image(s, x, y, i->px, i->w, i->h);
}

void ui_draw_glyph(surface_t *s, const char *name, int size, int x, int y, uint32_t color) {
    ui_image_t *i = ui_glyph(name, size);
    if (i) gfx_draw_image_tinted(s, x, y, i->px, i->w, i->h, color);
}

/* ------------------------------------------------------------------ windows */
static void alloc_surface(ui_window_t *win, int w, int h) {
    free(win->px);
    win->w = w;
    win->h = h;
    win->px = malloc((size_t)w * h * 4);
    gfx_init(&win->surf, win->px, w, h, w);
}

ui_window_t *ui_window_ex(const gui_wincreate_t *req) {
    ui_init();
    ui_window_t *win = calloc(1, sizeof(ui_window_t));
    alloc_surface(win, req->w, req->h);
    win->flags = req->flags;
    win->is_popup = (req->flags & WF_POPUP) != 0;
    gfx_fill(&win->surf, 0, 0, req->w, req->h, ui_bg());
    /* upload the first frame before the window becomes visible */
    gui_wincreate_t r = *req;
    bool hidden = r.flags & WF_HIDDEN;
    r.flags |= WF_HIDDEN;
    win->id = gui_win_create(&r);
    if (win->id < 0) { free(win->px); free(win); return 0; }
    win->alive = true;
    win->dirty = mkrect(0, 0, req->w, req->h);
    win->next = windows;
    windows = win;
    if (!hidden) {
        ui_flush(win);
        gui_win_action(win->id, WA_SHOW);
    }
    return win;
}

ui_window_t *ui_window(const char *title, int w, int h, uint32_t flags, const char *icon) {
    gui_wincreate_t req;
    memset(&req, 0, sizeof(req));
    req.x = -1;
    req.y = -1;
    req.w = w;
    req.h = h;
    req.flags = flags;
    req.min_w = MIN_W_DEFAULT(w);
    req.min_h = h < 200 ? h : 150;
    strlcpy(req.title, title, sizeof(req.title));
    if (icon) strlcpy(req.icon, icon, sizeof(req.icon));
    return ui_window_ex(&req);
}

static ui_window_t *graveyard;

/* windows are destroyed immediately on the server but freed later, so that
 * callbacks running inside the window's own event handler stay safe */
void ui_window_close(ui_window_t *win) {
    if (!win || !win->alive) return;
    win->alive = false;
    gui_win_destroy(win->id);
    for (ui_window_t **pp = &windows; *pp; pp = &(*pp)->next) {
        if (*pp == win) { *pp = win->next; break; }
    }
    win->next = graveyard;
    graveyard = win;
}

static void bury(void) {
    while (graveyard) {
        ui_window_t *win = graveyard;
        graveyard = win->next;
        for (ui_widget_t *w = win->widgets, *n; w; w = n) {
            n = w->next;
            ui_widget_free(w);
        }
        free(win->px);
        free(win);
    }
}

void ui_set_title(ui_window_t *win, const char *t) { gui_win_set_title(win->id, t); }

void ui_invalidate(ui_window_t *win) { win->dirty = mkrect(0, 0, win->w, win->h); }

void ui_invalidate_rect(ui_window_t *win, rect_t r) {
    rect_t c;
    if (rect_intersect(r, mkrect(0, 0, win->w, win->h), &c)) win->dirty = rect_union(win->dirty, c);
}

void ui_widget_invalidate(ui_widget_t *w) { if (w && w->win) ui_invalidate_rect(w->win, w->r); }

uint32_t ui_bg(void) { return ui_theme.window_bg; }
bool ui_is_retro(void) { return ui_theme.style == 1; }

void ui_flush(ui_window_t *win) {
    if (!win->alive || rect_empty(win->dirty)) return;
    rect_t d = win->dirty;
    win->dirty = mkrect(0, 0, 0, 0);
    surface_t *s = &win->surf;
    gfx_set_clip(s, d);
    gfx_fill(s, d.x, d.y, d.w, d.h, win->bg ? win->bg : ui_bg());
    if (win->on_paint) {
        win->on_paint(win, s);
        gfx_set_clip(s, d);
    }
    for (ui_widget_t *w = win->widgets; w; w = w->next) {
        if (!w->visible || !rect_intersect(w->r, d, 0)) continue;
        rect_t c;
        rect_intersect(w->r, d, &c);
        gfx_set_clip(s, c);
        ui_widget_draw(w, s);
    }
    gfx_reset_clip(s);
    gui_win_update(win->id, win->px, win->w, d.x, d.y, d.w, d.h);
}

static void relayout(ui_window_t *win) {
    for (ui_widget_t *w = win->widgets; w; w = w->next) {
        if ((w->anchor & A_RIGHT) && (w->anchor & A_LEFT)) w->r.w = MAX(0, win->w - w->r.x - w->mr);
        else if (w->anchor & A_RIGHT) w->r.x = win->w - w->mr - w->r.w;
        if ((w->anchor & A_BOTTOM) && (w->anchor & A_TOP)) w->r.h = MAX(0, win->h - w->r.y - w->mb);
        else if (w->anchor & A_BOTTOM) w->r.y = win->h - w->mb - w->r.h;
    }
}

void ui_resize(ui_window_t *win, int w, int h) {
    gui_win_resize(win->id, w, h);   /* the server answers with EV_RESIZE */
}

void ui_set_cursor(ui_window_t *win, int shape) { gui_win_set_cursor(win->id, shape); }

void ui_focus(ui_window_t *win, ui_widget_t *w) {
    if (win->focus == w) return;
    if (win->focus) ui_widget_invalidate(win->focus);
    win->focus = w;
    if (w) ui_widget_invalidate(w);
}

static ui_window_t *find_window(int id) {
    for (ui_window_t *w = windows; w; w = w->next)
        if (w->id == id) return w;
    return 0;
}

static ui_widget_t *widget_at(ui_window_t *win, int x, int y) {
    ui_widget_t *hit = 0;
    for (ui_widget_t *w = win->widgets; w; w = w->next)
        if (w->visible && rect_contains(w->r, x, y) && w->type != W_PANEL && w->type != W_LABEL && w->type != W_SEPARATOR)
            hit = w;
    return hit;
}

static void focus_next(ui_window_t *win, bool back) {
    ui_widget_t *list[128];
    int n = 0, cur = -1;
    for (ui_widget_t *w = win->widgets; w && n < 128; w = w->next) {
        if (w->visible && w->enabled && w->focusable) {
            if (w == win->focus) cur = n;
            list[n++] = w;
        }
    }
    if (!n) return;
    int next = cur < 0 ? 0 : (cur + (back ? n - 1 : 1)) % n;
    ui_focus(win, list[next]);
}

static void dispatch(ui_window_t *win, gui_event_t *ev) {
    switch (ev->type) {
    case EV_RESIZE:
        if (ev->w != win->w || ev->h != win->h) {
            alloc_surface(win, ev->w, ev->h);
            relayout(win);
            if (win->on_resize) win->on_resize(win);
        }
        ui_invalidate(win);
        break;
    case EV_CLOSE:
        if (!win->on_close || win->on_close(win)) ui_window_close(win);
        return;
    case EV_POPUP_CLOSE:
        if (win->is_popup) {
            if (win->on_event) win->on_event(win, ev);
            ui_window_close(win);
            return;
        }
        break;
    case EV_THEME:
        ui_reload_theme();
        for (ui_window_t *w = windows; w; w = w->next) ui_invalidate(w);
        break;
    case EV_FOCUS: win->focused = true; ui_invalidate(win); break;
    case EV_UNFOCUS: win->focused = false; ui_invalidate(win); break;
    case EV_MOUSE_MOVE: case EV_MOUSE_DOWN: case EV_MOUSE_UP: case EV_MOUSE_WHEEL: case EV_MOUSE_LEAVE: {
        ui_widget_t *target = win->capture ? win->capture : widget_at(win, ev->x, ev->y);
        if (ev->type == EV_MOUSE_LEAVE) target = 0;
        if (!win->capture && target != win->hover) {
            if (win->hover) {
                gui_event_t lv = *ev;
                lv.type = EV_MOUSE_LEAVE;
                ui_widget_event(win->hover, &lv);
                ui_widget_invalidate(win->hover);
            }
            win->hover = target;
            if (target) ui_widget_invalidate(target);
            int shape = CUR_ARROW;
            if (target && target->cursor) shape = target->cursor;
            else if (target && target->type == W_TEXTBOX) shape = CUR_TEXT;
            ui_set_cursor(win, shape);
        }
        if (ev->type == EV_MOUSE_DOWN) {
            if (target && target->focusable && target->enabled) ui_focus(win, target);
            if (target) win->capture = target;
        }
        if (target && target->enabled) ui_widget_event(target, ev);
        if (ev->type == EV_MOUSE_UP && !ev->buttons) win->capture = 0;
        if (!win->alive) return;
        break;
    }
    case EV_KEY_DOWN:
        if (ev->key == KEY_TAB && !(ev->mods & (MOD_CTRL | MOD_ALT)) &&
            !(win->focus && win->focus->type == W_CANVAS)) {
            focus_next(win, ev->mods & MOD_SHIFT);
            return;
        }
        if (win->focus && win->focus->enabled && win->focus->visible && ui_widget_event(win->focus, ev)) return;
        if (win->on_key) win->on_key(win, ev);
        break;
    case EV_KEY_UP:
        if (win->focus && win->focus->type == W_CANVAS) ui_widget_event(win->focus, ev);
        break;
    }
    if (win->alive && win->on_event) win->on_event(win, ev);
}

/* ------------------------------------------------------------------ timers & fd watches */
#define MAX_TIMERS 16
static struct { int id; int interval; uint64_t due; void (*cb)(void *); void *arg; } timers[MAX_TIMERS];
static int next_timer_id = 1;
#define MAX_WATCH 8
static struct { int fd; void (*cb)(int, void *); void *arg; } watches[MAX_WATCH];

int ui_timer(int interval, void (*cb)(void *), void *arg) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].id) {
            timers[i].id = next_timer_id++;
            timers[i].interval = interval < 1 ? 1 : interval;
            timers[i].due = uptime_ms() + timers[i].interval;
            timers[i].cb = cb;
            timers[i].arg = arg;
            return timers[i].id;
        }
    }
    return -1;
}

void ui_timer_cancel(int id) {
    for (int i = 0; i < MAX_TIMERS; i++) if (timers[i].id == id) timers[i].id = 0;
}

void ui_watch_fd(int fd, void (*cb)(int, void *), void *arg) {
    for (int i = 0; i < MAX_WATCH; i++) {
        if (!watches[i].cb) { watches[i].fd = fd; watches[i].cb = cb; watches[i].arg = arg; return; }
    }
}

void ui_unwatch_fd(int fd) {
    for (int i = 0; i < MAX_WATCH; i++) if (watches[i].cb && watches[i].fd == fd) watches[i].cb = 0;
}

void ui_quit(void) { quitting = true; }

static void flush_all(void) {
    for (ui_window_t *w = windows; w; w = w->next) ui_flush(w);
}

bool ui_step(int timeout) {
    bury();
    if (quitting || !windows) return false;
    flush_all();
    uint64_t now = uptime_ms();
    int wait = timeout;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].id) continue;
        int d = timers[i].due > now ? (int)(timers[i].due - now) : 0;
        if (wait < 0 || d < wait) wait = d;
    }
    kpollfd_t pfd[MAX_WATCH];
    int np = 0, map[MAX_WATCH];
    for (int i = 0; i < MAX_WATCH; i++) {
        if (!watches[i].cb) continue;
        pfd[np].fd = watches[i].fd;
        pfd[np].events = POLLIN;
        pfd[np].revents = 0;
        map[np++] = i;
    }
    gui_event_t ev;
    int got;
    if (np) {
        poll_ex(pfd, np, wait, POLL_GUI);
        for (int i = 0; i < np; i++) {
            int wi = map[i];
            if (pfd[i].revents && watches[wi].cb) watches[wi].cb(watches[wi].fd, watches[wi].arg);
        }
        got = gui_get_event(&ev, 0);
    } else {
        got = gui_get_event(&ev, wait);
    }
    int budget = 200;
    while (got > 0 && budget-- > 0) {
        ui_window_t *w = find_window(ev.win);
        if (w && !blocked_by_modal(w, &ev)) dispatch(w, &ev);
        got = gui_get_event(&ev, 0);
    }
    now = uptime_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].id && timers[i].due <= now) {
            timers[i].due = now + timers[i].interval;
            timers[i].cb(timers[i].arg);
        }
    }
    flush_all();
    return !quitting && windows;
}

void ui_run(void) {
    while (ui_step(-1)) {}
}

/* ------------------------------------------------------------------ drawing helpers */
void ui_bevel(surface_t *s, rect_t r, bool sunken) {
    uint32_t hi = 0xFFFFFFFF, lt = 0xFFDFDFDF, lo = 0xFF808080, dk = 0xFF000000;
    if (sunken) { uint32_t t = hi; hi = dk; dk = t; t = lt; lt = lo; lo = t; }
    gfx_hline(s, r.x, r.y, r.w - 1, lt);
    gfx_vline(s, r.x, r.y, r.h - 1, lt);
    gfx_hline(s, r.x + 1, r.y + 1, r.w - 3, hi);
    gfx_vline(s, r.x + 1, r.y + 1, r.h - 3, hi);
    gfx_hline(s, r.x, r.y + r.h - 1, r.w, dk);
    gfx_vline(s, r.x + r.w - 1, r.y, r.h, dk);
    gfx_hline(s, r.x + 1, r.y + r.h - 2, r.w - 2, lo);
    gfx_vline(s, r.x + r.w - 2, r.y + 1, r.h - 2, lo);
}

void ui_draw_button_frame(surface_t *s, rect_t r, int state, bool primary) {
    if (ui_is_retro()) {
        gfx_fill(s, r.x, r.y, r.w, r.h, ui_theme.button_bg);
        ui_bevel(s, r, state == 2);
        if (primary) gfx_rect(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, 0xFF000000);
        return;
    }
    uint32_t bg, border;
    if (primary) {
        bg = state == 2 ? gfx_darken(ui_theme.accent, 40) : state == 1 ? gfx_lighten(ui_theme.accent, 25) : ui_theme.accent;
        border = gfx_darken(ui_theme.accent, 30);
    } else {
        bg = state == 2 ? ui_theme.button_pressed : state == 1 ? ui_theme.button_hover : ui_theme.button_bg;
        border = ui_theme.button_border;
    }
    if (state == 3) bg = gfx_mix(bg, ui_bg(), 120);
    gfx_fill_rounded(s, r.x, r.y, r.w, r.h, 5, bg);
    gfx_rounded_rect(s, r.x, r.y, r.w, r.h, 5, border);
}

void ui_draw_input_frame(surface_t *s, rect_t r, bool focused) {
    if (ui_is_retro()) {
        gfx_fill(s, r.x, r.y, r.w, r.h, ui_theme.input_bg);
        ui_bevel(s, r, true);
        return;
    }
    gfx_fill_rounded(s, r.x, r.y, r.w, r.h, 5, ui_theme.input_bg);
    gfx_rounded_rect(s, r.x, r.y, r.w, r.h, 5, focused ? ui_theme.accent : ui_theme.input_border);
    if (focused) gfx_hline(s, r.x + 4, r.y + r.h - 2, r.w - 8, ui_theme.accent);
}

void ui_draw_scrollbar(surface_t *s, rect_t t, int total, int visible, int pos, bool hover) {
    if (total <= visible) return;
    bool vertical = t.h >= t.w;
    int len = vertical ? t.h : t.w;
    int thumb = MAX(24, len * visible / total);
    int range = total - visible;
    int off = range > 0 ? (len - thumb) * pos / range : 0;
    if (ui_is_retro()) {
        gfx_fill(s, t.x, t.y, t.w, t.h, ui_theme.scroll_track);
        rect_t th = vertical ? mkrect(t.x, t.y + off, t.w, thumb) : mkrect(t.x + off, t.y, thumb, t.h);
        gfx_fill(s, th.x, th.y, th.w, th.h, ui_theme.scroll_thumb);
        ui_bevel(s, th, false);
        return;
    }
    int thick = hover ? 8 : 6;
    if (vertical) {
        int x = t.x + t.w - thick - 2;
        gfx_fill_rounded(s, x, t.y + off + 2, thick, thumb - 4, thick / 2,
                         hover ? gfx_mix(ui_theme.scroll_thumb, ui_theme.window_text, 60) : ui_theme.scroll_thumb);
    } else {
        int y = t.y + t.h - thick - 2;
        gfx_fill_rounded(s, t.x + off + 2, y, thumb - 4, thick, thick / 2, ui_theme.scroll_thumb);
    }
}

void ui_draw_text_center(surface_t *s, const font_t *f, rect_t r, const char *text, uint32_t color) {
    int tw = font_text_width(f, text);
    int x = r.x + (r.w - tw) / 2;
    if (tw > r.w) { font_draw_fit(s, f, r.x + 2, r.y + (r.h - f->height) / 2, text, r.w - 4, color); return; }
    font_draw(s, f, x, r.y + (r.h - f->height) / 2, text, color);
}
