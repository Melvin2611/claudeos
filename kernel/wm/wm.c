/* ClaudeOS window server: windows, compositing, decorations, input routing, GUI system calls */
#include "wm.h"
#include <vfs.h>
#include <proc.h>
#include <syscall.h>
#include <mm.h>
#include <boot.h>
#include <claudeos/image.h>

wm_state_t wm;

#define MAX_DIRTY 48
static rect_t dirty[MAX_DIRTY];
static int ndirty;

enum { PART_NONE, PART_CONTENT, PART_TITLE, PART_CLOSE, PART_MAX, PART_MIN, PART_EDGE };
#define EDGE_L 1
#define EDGE_R 2
#define EDGE_T 4
#define EDGE_B 8
#define GRIP 6

enum { DR_NONE, DR_MOVE, DR_RESIZE, DR_CAPTURE };
static struct {
    int mode;
    window_t *win;
    int edges;
    int ox, oy;
    rect_t start;
    int smx, smy;
    int snap;            /* preview: 0 none, 1 left, 2 right, 3 maximize */
    bool moved;
} drag;

static window_t *hover_win;
static int hover_part;
static window_t *pressed_win;
static int pressed_part;
static window_t *mouse_over;
static uint64_t last_click_ms;
static int last_click_x, last_click_y, click_count;
static uint32_t last_click_btn;
static char *clipboard;
static size_t clipboard_len;
static bool super_alone;

/* ------------------------------------------------------------------ geometry */

int win_border(void) { return wm.theme.style == 1 ? 4 : 1; }
int win_title_h(void) { return wm.theme.style == 1 ? 24 : wm.theme.title_h; }

rect_t win_frame(window_t *w) {
    if (w->flags & WF_NO_DECOR) return mkrect(w->x, w->y, w->cw, w->ch);
    int b = win_border();
    return mkrect(w->x, w->y, w->cw + 2 * b, w->ch + win_title_h() + b);
}

rect_t win_content(window_t *w) {
    if (w->flags & WF_NO_DECOR) return mkrect(w->x, w->y, w->cw, w->ch);
    return mkrect(w->x + win_border(), w->y + win_title_h(), w->cw, w->ch);
}

static bool has_shadow(window_t *w) {
    return wm.theme.shadows && (!(w->flags & WF_NO_DECOR) || (w->flags & (WF_SHADOW | WF_POPUP)));
}

static rect_t win_bounds(window_t *w) {
    rect_t f = win_frame(w);
    if (!has_shadow(w)) return f;
    int s = 26;
    return mkrect(f.x - s, f.y - s, f.w + 2 * s, f.h + 2 * s + 8);
}

rect_t wm_work_area(void) {
    rect_t tb = shell_taskbar_rect();
    if (wm.cfg.taskbar_top) return mkrect(0, tb.h, wm.w, wm.h - tb.h);
    return mkrect(0, 0, wm.w, wm.h - tb.h);
}

static rect_t btn_rect(window_t *w, int part) {
    rect_t f = win_frame(w);
    if (wm.theme.style == 1) {
        int bw = 16, bh = 14, y = f.y + 4 + 2;
        int x = f.x + f.w - 4 - 2 - bw;
        if (part == PART_CLOSE) return mkrect(x, y, bw, bh);
        x -= bw + 2;
        if (!(w->flags & (WF_DIALOG | WF_NO_MAXIMIZE)) && (w->flags & WF_RESIZABLE)) {
            if (part == PART_MAX) return mkrect(x, y, bw, bh);
            x -= bw;
        }
        if (part == PART_MIN && !(w->flags & WF_DIALOG)) return mkrect(x, y, bw, bh);
        return mkrect(0, 0, 0, 0);
    }
    int bw = 46, bh = win_title_h() - 1;
    int x = f.x + f.w - 1 - bw;
    if (part == PART_CLOSE) return mkrect(x, f.y + 1, bw, bh);
    x -= bw;
    if (!(w->flags & (WF_DIALOG | WF_NO_MAXIMIZE)) && (w->flags & WF_RESIZABLE)) {
        if (part == PART_MAX) return mkrect(x, f.y + 1, bw, bh);
        x -= bw;
    }
    if (part == PART_MIN && !(w->flags & WF_DIALOG)) return mkrect(x, f.y + 1, bw, bh);
    return mkrect(0, 0, 0, 0);
}

/* ------------------------------------------------------------------ damage */

void wm_damage(rect_t r) {
    if (!rect_intersect(r, mkrect(0, 0, wm.w, wm.h), &r)) return;
    for (int i = 0; i < ndirty; i++) {
        rect_t u = rect_union(dirty[i], r);
        /* merge when the union does not waste much area */
        if ((int64_t)u.w * u.h <= (int64_t)dirty[i].w * dirty[i].h + (int64_t)r.w * r.h + 4096) {
            dirty[i] = u;
            return;
        }
    }
    if (ndirty < MAX_DIRTY) dirty[ndirty++] = r;
    else {
        for (int i = 1; i < ndirty; i++) dirty[0] = rect_union(dirty[0], dirty[i]);
        dirty[0] = rect_union(dirty[0], r);
        ndirty = 1;
    }
}

void wm_damage_all(void) {
    ndirty = 0;
    wm_damage(mkrect(0, 0, wm.w, wm.h));
}

static void damage_win(window_t *w) { wm_damage(win_bounds(w)); }

/* ------------------------------------------------------------------ drawing */

static void draw_button_glyph(surface_t *s, int part, rect_t b, uint32_t col, bool maximized) {
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    if (part == PART_MIN) {
        gfx_hline(s, cx - 5, cy, 10, col);
    } else if (part == PART_MAX) {
        if (maximized) {
            gfx_rect(s, cx - 5, cy - 3, 8, 8, col);
            gfx_hline(s, cx - 3, cy - 5, 8, col);
            gfx_vline(s, cx + 4, cy - 5, 8, col);
        } else {
            gfx_rect(s, cx - 5, cy - 5, 10, 10, col);
        }
    } else if (part == PART_CLOSE) {
        gfx_line_aa(s, cx - 5, cy - 5, cx + 5, cy + 5, col);
        gfx_line_aa(s, cx + 5, cy - 5, cx - 5, cy + 5, col);
        gfx_line_aa(s, cx - 5, cy - 4, cx + 4, cy + 5, WITH_ALPHA(col, 90));
        gfx_line_aa(s, cx + 4, cy - 5, cx - 5, cy + 4, WITH_ALPHA(col, 90));
    }
}

static void bevel(surface_t *s, rect_t r, bool sunken) {
    uint32_t hi = 0xFFFFFFFF, lo = 0xFF808080, dk = 0xFF000000, lt = 0xFFDFDFDF;
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

static void draw_decor_retro(surface_t *s, window_t *w, bool active) {
    rect_t f = win_frame(w);
    gfx_fill(s, f.x, f.y, f.w, f.h, wm.theme.window_bg);
    bevel(s, f, false);
    rect_t tb = mkrect(f.x + 4, f.y + 4, f.w - 8, 18);
    uint32_t c1 = active ? wm.theme.title_bg : wm.theme.title_bg_inactive;
    uint32_t c2 = active ? 0xFF1084D0 : 0xFFB5B5B5;
    gfx_gradient_h(s, tb.x, tb.y, tb.w, tb.h, c1, c2);
    icon_t *ic = icon_get(w->icon[0] ? w->icon : "file-exec", 16);
    int tx = tb.x + 3;
    if (ic) { draw_icon(s, ic, tx, tb.y + 1); tx += 20; }
    font_draw_fit(s, F_UIB, tx, tb.y + 2, w->title, tb.w - (tx - tb.x) - 60,
                  active ? wm.theme.title_text : wm.theme.title_text_inactive);
    for (int part = PART_CLOSE; part <= PART_MIN; part++) {
        rect_t b = btn_rect(w, part);
        if (!b.w) continue;
        bool pressed = pressed_win == w && pressed_part == part && hover_win == w && hover_part == part;
        gfx_fill(s, b.x, b.y, b.w, b.h, 0xFFC0C0C0);
        bevel(s, b, pressed);
        int o = pressed ? 1 : 0;
        rect_t g = mkrect(b.x + o, b.y + o, b.w, b.h);
        if (part == PART_CLOSE) {
            for (int i = 0; i < 2; i++) {
                gfx_line(s, g.x + 4 + i, g.y + 3, g.x + 10 + i, g.y + 9, 0xFF000000);
                gfx_line(s, g.x + 10 + i, g.y + 3, g.x + 4 + i, g.y + 9, 0xFF000000);
            }
        } else if (part == PART_MAX) {
            gfx_rect(s, g.x + 3, g.y + 2, 9, 9, 0xFF000000);
            gfx_hline(s, g.x + 3, g.y + 3, 9, 0xFF000000);
        } else {
            gfx_fill(s, g.x + 4, g.y + 9, 6, 2, 0xFF000000);
        }
    }
}

static void draw_decor(surface_t *s, window_t *w, bool active) {
    if (wm.theme.style == 1) { draw_decor_retro(s, w, active); return; }
    rect_t f = win_frame(w);
    int r = w->maximized || w->snapped ? 0 : wm.theme.radius;
    int th = win_title_h();
    uint32_t tbg = active ? wm.theme.title_bg : wm.theme.title_bg_inactive;
    /* title bar: rounded top corners, square bottom */
    gfx_fill_rounded(s, f.x, f.y, f.w, th + r + 1, r, tbg);
    /* subtle accent line on the active window */
    if (active) {
        gfx_fill(s, f.x + r, f.y, f.w - 2 * r, 1, WITH_ALPHA(wm.theme.accent, 160));
    }
    icon_t *ic = icon_get(w->icon[0] ? w->icon : "file-exec", 16);
    int tx = f.x + 12;
    if (ic) { draw_icon(s, ic, tx, f.y + (th - 16) / 2); tx += 26; }
    int bx = f.x + f.w;
    for (int part = PART_CLOSE; part <= PART_MIN; part++) {
        rect_t b = btn_rect(w, part);
        if (b.w && b.x < bx) bx = b.x;
    }
    uint32_t tcol = active ? wm.theme.title_text : wm.theme.title_text_inactive;
    font_draw_fit(s, F_UI, tx, f.y + (th - F_UI->height) / 2, w->title, bx - tx - 8, tcol);
    for (int part = PART_CLOSE; part <= PART_MIN; part++) {
        rect_t b = btn_rect(w, part);
        if (!b.w) continue;
        bool hover = hover_win == w && hover_part == part;
        bool pressed = hover && pressed_win == w && pressed_part == part;
        uint32_t gcol = tcol;
        if (hover) {
            uint32_t hb;
            if (part == PART_CLOSE) { hb = pressed ? 0xFFB4101E : 0xFFE81123; gcol = 0xFFFFFFFF; }
            else hb = wm.theme.dark ? gfx_lighten(tbg, pressed ? 40 : 25) : gfx_darken(tbg, pressed ? 40 : 22);
            if (part == PART_CLOSE && r > 0) {
                /* keep the rounded top-right corner */
                surface_t tmp = *s;
                rect_t cl;
                if (rect_intersect(s->clip, b, &cl)) {
                    gfx_set_clip(&tmp, cl);
                    gfx_fill_rounded(&tmp, b.x - r, b.y - 1, b.w + r, b.h + r + 1, r, hb);
                    gfx_fill(&tmp, b.x, b.y + r, b.w, b.h - r, hb);
                    gfx_fill(&tmp, b.x - r, b.y - 1, r, b.h + 1, tbg);
                }
            } else {
                gfx_fill(s, b.x, b.y, b.w, b.h, hb);
            }
        }
        draw_button_glyph(s, part, b, gcol, w->maximized);
    }
}

static void draw_window(surface_t *s, window_t *w) {
    bool active = wm.focused == w;
    rect_t f = win_frame(w);
    bool decor = !(w->flags & WF_NO_DECOR);
    int r = (w->maximized || w->snapped || wm.theme.style == 1) ? 0 : wm.theme.radius;
    if (!decor) r = (w->flags & WF_POPUP) ? MIN(wm.theme.radius, 8) : 0;
    if (has_shadow(w) && !w->maximized) {
        rect_t cl;
        if (rect_intersect(s->clip, win_bounds(w), &cl))
            gfx_shadow(s, f, r, active ? 22 : 14, active ? 120 : 80, active ? 6 : 3);
    }
    if (decor) draw_decor(s, w, active);
    rect_t c = win_content(w);
    surface_t src;
    gfx_init(&src, w->buf, w->cw, w->ch, w->cw);
    if (w->flags & WF_ALPHA) {
        gfx_blit_alpha(s, c.x, c.y, &src, 0, 0, w->cw, w->ch);
    } else if (r > 0) {
        gfx_blit_rounded(s, c.x, c.y, &src, w->cw, w->ch, decor ? MAX(r - 1, 0) : r, decor ? 12 : 15);
    } else {
        gfx_blit(s, c.x, c.y, &src, 0, 0, w->cw, w->ch);
    }
    if (decor && wm.theme.style == 0) {
        uint32_t bc = active ? gfx_mix(wm.theme.border, wm.theme.accent, 110) : wm.theme.border_inactive;
        if (w->maximized) {
            /* no outline */
        } else {
            gfx_rounded_rect(s, f.x, f.y, f.w, f.h, r, bc);
        }
    } else if (!decor && (w->flags & WF_POPUP)) {
        gfx_rounded_rect(s, f.x, f.y, f.w, f.h, r, wm.theme.menu_border);
    }
}

static void draw_snap_preview(surface_t *s) {
    if (drag.mode != DR_MOVE || !drag.snap) return;
    rect_t wa = wm_work_area();
    rect_t p = wa;
    if (drag.snap == 1) p.w = wa.w / 2;
    else if (drag.snap == 2) { p.x = wa.x + wa.w / 2; p.w = wa.w - wa.w / 2; }
    p = mkrect(p.x + 8, p.y + 8, p.w - 16, p.h - 16);
    gfx_fill_rounded(s, p.x, p.y, p.w, p.h, 12, WITH_ALPHA(wm.theme.accent, 70));
    gfx_rounded_rect(s, p.x, p.y, p.w, p.h, 12, WITH_ALPHA(wm.theme.accent, 200));
}

static rect_t cursor_rect(void) {
    int hx, hy;
    icon_t *ic = cursor_icon(wm.cursor_shape, &hx, &hy);
    if (!ic) return mkrect(wm.mx, wm.my, 12, 18);
    return mkrect(wm.mx - hx, wm.my - hy, ic->w, ic->h);
}

static void draw_cursor(surface_t *s) {
    int hx, hy;
    icon_t *ic = cursor_icon(wm.cursor_shape, &hx, &hy);
    if (ic) {
        draw_icon(s, ic, wm.mx - hx, wm.my - hy);
    } else {
        gfx_fill_triangle(s, wm.mx, wm.my, wm.mx, wm.my + 16, wm.mx + 11, wm.my + 11, 0xFFFFFFFF);
    }
}

static void compose_rect(rect_t r) {
    surface_t *s = &wm.scr;
    gfx_set_clip(s, r);
    wallpaper_draw(s, r);
    shell_draw(s, r, 0);
    for (window_t *w = wm.windows; w; w = w->next) {
        if (!w->visible || w->minimized || (w->flags & (WF_POPUP | WF_TOPMOST))) continue;
        if (rect_intersect(win_bounds(w), r, 0)) draw_window(s, w);
    }
    draw_snap_preview(s);
    shell_draw(s, r, 1);
    for (window_t *w = wm.windows; w; w = w->next) {
        if (!w->visible || w->minimized || !(w->flags & (WF_POPUP | WF_TOPMOST))) continue;
        if (rect_intersect(win_bounds(w), r, 0)) draw_window(s, w);
    }
    shell_draw(s, r, 2);
    if (rect_intersect(cursor_rect(), r, 0)) draw_cursor(s);
    gfx_reset_clip(s);
}

static void compose(void) {
    if (!ndirty) return;
    int n = ndirty;
    rect_t rects[MAX_DIRTY];
    memcpy(rects, dirty, sizeof(rect_t) * n);
    ndirty = 0;
    for (int i = 0; i < n; i++) {
        compose_rect(rects[i]);
        fb_flush(&wm.scr, rects[i]);
    }
}

/* ------------------------------------------------------------------ clients & events */

static gui_client_t *client_of(task_t *t) {
    if (!t->gui) {
        gui_client_t *c = kzalloc(sizeof(gui_client_t));
        c->task = t;
        c->pid = t->pid;
        t->gui = c;
    }
    return t->gui;
}

void wm_send(window_t *w, gui_event_t *ev) {
    gui_client_t *c = w->client;
    if (!c) return;
    ev->win = w->id;
    uint64_t f = irq_save();
    /* coalesce motion and resize events */
    if (c->head != c->tail && (ev->type == EV_MOUSE_MOVE || ev->type == EV_RESIZE)) {
        int last = (c->head + EVQ_SIZE - 1) % EVQ_SIZE;
        if (c->q[last].type == ev->type && c->q[last].win == ev->win) {
            c->q[last] = *ev;
            irq_restore(f);
            return;
        }
    }
    int next = (c->head + 1) % EVQ_SIZE;
    if (next != c->tail) {
        c->q[c->head] = *ev;
        c->head = next;
    }
    wq_wake_all(&c->wq);
    irq_restore(f);
    poll_notify();
}

static void send_simple(window_t *w, int type) {
    gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    wm_send(w, &ev);
}

bool gui_event_pending(task_t *t) {
    gui_client_t *c = t->gui;
    return c && c->head != c->tail;
}

int gui_count_windows(task_t *t) {
    gui_client_t *c = t->gui;
    return c ? c->nwindows : 0;
}

window_t *win_find(int id) {
    for (window_t *w = wm.windows; w; w = w->next)
        if (w->id == id) return w;
    return 0;
}

static window_t *own_window(int id) {
    window_t *w = win_find(id);
    if (!w || !w->client || w->client->task != current) return 0;
    return w;
}

static window_t *top_visible(void) {
    window_t *best = 0;
    for (window_t *w = wm.windows; w; w = w->next)
        if (w->visible && !w->minimized && !(w->flags & WF_POPUP)) best = w;
    return best;
}

void win_focus(window_t *w) {
    if (wm.focused == w) return;
    window_t *old = wm.focused;
    wm.focused = w;
    if (old) { send_simple(old, EV_UNFOCUS); damage_win(old); }
    if (w) { send_simple(w, EV_FOCUS); damage_win(w); }
    shell_windows_changed();
}

void win_raise(window_t *w) {
    /* unlink and append at the top (popups/topmost stay above normal windows) */
    window_t **pp = &wm.windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (!*pp) return;
    *pp = w->next;
    w->next = 0;
    pp = &wm.windows;
    while (*pp) pp = &(*pp)->next;
    *pp = w;
    damage_win(w);
}

void win_minimize(window_t *w) {
    if (w->minimized) return;
    damage_win(w);
    w->minimized = true;
    send_simple(w, EV_MINIMIZE);
    if (wm.focused == w) {
        wm.focused = 0;
        send_simple(w, EV_UNFOCUS);
        window_t *t = top_visible();
        if (t) win_focus(t);
    }
    shell_windows_changed();
}

void win_restore(window_t *w) {
    if (w->minimized) {
        w->minimized = false;
        send_simple(w, EV_RESTORE);
    }
    win_raise(w);
    win_focus(w);
    damage_win(w);
    shell_windows_changed();
}

/* change the content size (server side) and tell the client */
static void win_set_size(window_t *w, int cw, int ch) {
    cw = MAX(cw, MAX(w->min_w, 60));
    ch = MAX(ch, MAX(w->min_h, 20));
    cw = MIN(cw, 4096);
    ch = MIN(ch, 4096);
    if (cw == w->cw && ch == w->ch) return;
    damage_win(w);
    uint32_t *nb = vmalloc((size_t)cw * ch * 4);
    if (!nb) return;
    uint32_t bg = wm.theme.window_bg;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++)
            nb[y * cw + x] = (y < w->ch && x < w->cw) ? w->buf[y * w->cw + x] : bg;
    }
    vfree(w->buf);
    w->buf = nb;
    w->cw = cw;
    w->ch = ch;
    damage_win(w);
    gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_RESIZE;
    ev.w = cw;
    ev.h = ch;
    wm_send(w, &ev);
}

static void win_set_frame(window_t *w, rect_t fr) {
    damage_win(w);
    int b = (w->flags & WF_NO_DECOR) ? 0 : win_border();
    int th = (w->flags & WF_NO_DECOR) ? 0 : win_title_h();
    w->x = fr.x;
    w->y = fr.y;
    win_set_size(w, fr.w - 2 * b, fr.h - th - b);
    damage_win(w);
}

void win_toggle_maximize(window_t *w) {
    if (!(w->flags & WF_RESIZABLE) || (w->flags & WF_NO_MAXIMIZE)) return;
    if (w->maximized || w->snapped) {
        w->maximized = false;
        w->snapped = 0;
        win_set_frame(w, w->restore);
    } else {
        w->restore = win_frame(w);
        w->maximized = true;
        win_set_frame(w, wm_work_area());
    }
    shell_windows_changed();
}

static void win_snap(window_t *w, int side) {
    if (!(w->flags & WF_RESIZABLE)) return;
    rect_t wa = wm_work_area();
    if (!w->maximized && !w->snapped) w->restore = win_frame(w);
    if (side == 3) {
        w->snapped = 0;
        w->maximized = true;
        win_set_frame(w, wa);
        return;
    }
    w->maximized = false;
    w->snapped = side;
    rect_t r = wa;
    r.w = wa.w / 2;
    if (side == 2) { r.x = wa.x + wa.w / 2; r.w = wa.w - wa.w / 2; }
    win_set_frame(w, r);
}

void win_close_request(window_t *w) { send_simple(w, EV_CLOSE); }

static void win_destroy(window_t *w) {
    damage_win(w);
    window_t **pp = &wm.windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    if (hover_win == w) hover_win = 0;
    if (pressed_win == w) pressed_win = 0;
    if (mouse_over == w) mouse_over = 0;
    if (drag.win == w) { drag.mode = DR_NONE; drag.win = 0; }
    if (w->client) w->client->nwindows--;
    bool was_focused = wm.focused == w;
    if (was_focused) wm.focused = 0;
    vfree(w->buf);
    int parent = w->parent;
    kfree(w);
    if (was_focused) {
        window_t *p = parent ? win_find(parent) : 0;
        if (p && p->visible && !p->minimized) win_focus(p);
        else {
            window_t *t = top_visible();
            if (t) win_focus(t);
        }
    }
    shell_windows_changed();
}

void gui_proc_exit(task_t *t) {
    if (!wm.ready || !t->gui) return;
    mutex_lock(&wm.lock);
    for (window_t *w = wm.windows, *n; w; w = n) {
        n = w->next;
        if (w->client && w->client->task == t) win_destroy(w);
    }
    kfree(t->gui);
    t->gui = 0;
    mutex_unlock(&wm.lock);
    input_kick();
}

/* ------------------------------------------------------------------ hit testing */

static int hit_part(window_t *w, int x, int y, int *edges) {
    rect_t f = win_frame(w);
    *edges = 0;
    bool resizable = (w->flags & WF_RESIZABLE) && !w->maximized && !(w->flags & WF_NO_DECOR);
    rect_t grip = resizable ? mkrect(f.x - GRIP, f.y - GRIP, f.w + 2 * GRIP, f.h + 2 * GRIP) : f;
    if (!rect_contains(grip, x, y)) return PART_NONE;
    if (resizable) {
        int e = 0;
        if (x < f.x + 3) e |= EDGE_L;
        if (x >= f.x + f.w - 3) e |= EDGE_R;
        if (y < f.y + 3) e |= EDGE_T;
        if (y >= f.y + f.h - 3) e |= EDGE_B;
        /* corners are easier to grab */
        if (e & (EDGE_L | EDGE_R)) { if (y < f.y + 16) e |= EDGE_T; if (y >= f.y + f.h - 16) e |= EDGE_B; }
        if (e & (EDGE_T | EDGE_B)) { if (x < f.x + 16) e |= EDGE_L; if (x >= f.x + f.w - 16) e |= EDGE_R; }
        if (e) { *edges = e; return PART_EDGE; }
    }
    if (w->flags & WF_NO_DECOR) return PART_CONTENT;
    rect_t c = win_content(w);
    if (rect_contains(c, x, y)) return PART_CONTENT;
    for (int part = PART_CLOSE; part <= PART_MIN; part++) {
        rect_t b = btn_rect(w, part);
        if (b.w && rect_contains(b, x, y)) return part;
    }
    if (y < f.y + win_title_h()) return PART_TITLE;
    return PART_TITLE;   /* borders act like the title (move) */
}

static window_t *window_at(int x, int y, int *part, int *edges) {
    window_t *hit = 0;
    int hp = PART_NONE, he = 0;
    /* two passes: topmost/popup layer first */
    for (int pass = 0; pass < 2 && !hit; pass++) {
        for (window_t *w = wm.windows; w; w = w->next) {
            if (!w->visible || w->minimized) continue;
            bool top = (w->flags & (WF_POPUP | WF_TOPMOST)) != 0;
            if ((pass == 0) != top) continue;
            int e;
            int p = hit_part(w, x, y, &e);
            if (p != PART_NONE) { hit = w; hp = p; he = e; }
        }
    }
    *part = hp;
    *edges = he;
    return hit;
}

static int edge_cursor(int e) {
    if ((e & (EDGE_L | EDGE_T)) == (EDGE_L | EDGE_T) || (e & (EDGE_R | EDGE_B)) == (EDGE_R | EDGE_B)) return CUR_RESIZE_NWSE;
    if ((e & (EDGE_R | EDGE_T)) == (EDGE_R | EDGE_T) || (e & (EDGE_L | EDGE_B)) == (EDGE_L | EDGE_B)) return CUR_RESIZE_NESW;
    if (e & (EDGE_L | EDGE_R)) return CUR_RESIZE_H;
    return CUR_RESIZE_V;
}

static void set_cursor(int shape) {
    if (shape == wm.cursor_shape) return;
    wm_damage(cursor_rect());
    wm.cursor_shape = shape;
    wm_damage(cursor_rect());
}

static void mouse_event_to(window_t *w, int type, uint32_t button, int clicks, int wheel) {
    rect_t c = win_content(w);
    gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.x = wm.mx - c.x;
    ev.y = wm.my - c.y;
    ev.sx = wm.mx;
    ev.sy = wm.my;
    ev.buttons = wm.buttons;
    ev.button = button;
    ev.clicks = clicks;
    ev.wheel = wheel;
    wm_send(w, &ev);
}

/* ------------------------------------------------------------------ mouse */

static void close_popups_except(window_t *keep) {
    for (window_t *w = wm.windows; w; w = w->next) {
        if ((w->flags & WF_POPUP) && w->visible && w != keep) {
            w->visible = false;
            damage_win(w);
            send_simple(w, EV_POPUP_CLOSE);
        }
    }
}

static bool any_popup(void) {
    for (window_t *w = wm.windows; w; w = w->next)
        if ((w->flags & WF_POPUP) && w->visible) return true;
    return false;
}

static void update_hover(void) {
    int part, edges;
    window_t *w = 0;
    if (drag.mode == DR_NONE) {
        w = window_at(wm.mx, wm.my, &part, &edges);
        if (rect_contains(shell_taskbar_rect(), wm.mx, wm.my) && !(w && (w->flags & (WF_POPUP | WF_TOPMOST))))
            w = 0, part = PART_NONE;
    } else {
        part = PART_NONE;
    }
    int hp = (part >= PART_CLOSE && part <= PART_MIN) ? part : PART_NONE;
    if (w != hover_win || hp != hover_part) {
        if (hover_win && hover_part) damage_win(hover_win);
        hover_win = hp ? w : 0;
        hover_part = hp;
        if (hover_win) damage_win(hover_win);
    }
    if (drag.mode == DR_RESIZE) return;
    if (drag.mode == DR_MOVE) { set_cursor(CUR_ARROW); return; }
    if (drag.mode == DR_CAPTURE && drag.win) { set_cursor(drag.win->cursor); return; }
    if (w && part == PART_EDGE) set_cursor(edge_cursor(edges));
    else if (w && part == PART_CONTENT) set_cursor(w->cursor);
    else set_cursor(CUR_ARROW);
    if (w != mouse_over) {
        if (mouse_over) mouse_event_to(mouse_over, EV_MOUSE_LEAVE, 0, 0, 0);
        mouse_over = (w && part == PART_CONTENT) ? w : 0;
    } else if (w && part != PART_CONTENT && mouse_over == w) {
        mouse_event_to(w, EV_MOUSE_LEAVE, 0, 0, 0);
        mouse_over = 0;
    }
}

static void do_drag_motion(void) {
    window_t *w = drag.win;
    if (!w) return;
    if (drag.mode == DR_MOVE) {
        int dx = wm.mx - drag.smx, dy = wm.my - drag.smy;
        if (!drag.moved && dx * dx + dy * dy < 16) return;
        drag.moved = true;
        if (w->maximized || w->snapped) {
            /* un-maximize while dragging: keep the cursor at the same relative title position */
            rect_t f = win_frame(w);
            int rel = f.w ? (wm.mx - f.x) * 1000 / f.w : 500;
            w->maximized = false;
            w->snapped = 0;
            rect_t rs = w->restore;
            rs.x = wm.mx - rs.w * rel / 1000;
            rs.y = wm.my - win_title_h() / 2;
            win_set_frame(w, rs);
            drag.ox = wm.mx - w->x;
            drag.oy = wm.my - w->y;
            shell_windows_changed();
        }
        damage_win(w);
        rect_t wa = wm_work_area();
        w->x = wm.mx - drag.ox;
        w->y = MAX(wa.y, MIN(wm.my - drag.oy, wa.y + wa.h - 24));
        damage_win(w);
        int snap = 0;
        if (w->flags & WF_RESIZABLE) {
            if (wm.mx <= 1) snap = 1;
            else if (wm.mx >= wm.w - 2) snap = 2;
            else if (wm.my <= wa.y + 1) snap = 3;
        }
        if (snap != drag.snap) {
            wm_damage(wa);
            drag.snap = snap;
        }
    } else if (drag.mode == DR_RESIZE) {
        int dx = wm.mx - drag.smx, dy = wm.my - drag.smy;
        rect_t r = drag.start;
        int minw = MAX(w->min_w, 120) + 2 * win_border();
        int minh = MAX(w->min_h, 40) + win_title_h() + win_border();
        if (drag.edges & EDGE_R) r.w = MAX(minw, drag.start.w + dx);
        if (drag.edges & EDGE_B) r.h = MAX(minh, drag.start.h + dy);
        if (drag.edges & EDGE_L) {
            r.w = MAX(minw, drag.start.w - dx);
            r.x = drag.start.x + drag.start.w - r.w;
        }
        if (drag.edges & EDGE_T) {
            r.h = MAX(minh, drag.start.h - dy);
            r.y = drag.start.y + drag.start.h - r.h;
        }
        win_set_frame(w, r);
    } else if (drag.mode == DR_CAPTURE) {
        mouse_event_to(w, EV_MOUSE_MOVE, 0, 0, 0);
    }
}

static void mouse_moved(void) {
    if (drag.mode != DR_NONE) {
        do_drag_motion();
        update_hover();
        return;
    }
    if (shell_mouse(EV_MOUSE_MOVE, wm.mx, wm.my, 0, 0, 0)) {
        update_hover();
        return;
    }
    update_hover();
    if (mouse_over) mouse_event_to(mouse_over, EV_MOUSE_MOVE, 0, 0, 0);
}

static void button_down(uint32_t btn) {
    uint64_t now = uptime_ms();
    if (btn == last_click_btn && now - last_click_ms < wm.cfg.double_click_ms &&
        ABS_DIFF(wm.mx, last_click_x) < 5 && ABS_DIFF(wm.my, last_click_y) < 5)
        click_count++;
    else
        click_count = 1;
    last_click_ms = now;
    last_click_x = wm.mx;
    last_click_y = wm.my;
    last_click_btn = btn;

    int part, edges;
    window_t *w = window_at(wm.mx, wm.my, &part, &edges);
    bool on_popup = w && (w->flags & WF_POPUP);
    if (any_popup() && !on_popup) {
        close_popups_except(0);
        return;
    }
    if (!on_popup && shell_overlay_open()) {
        if (shell_mouse(EV_MOUSE_DOWN, wm.mx, wm.my, btn, click_count, 0)) return;
    }
    bool top_layer = w && (w->flags & (WF_POPUP | WF_TOPMOST));
    if (!top_layer && rect_contains(shell_taskbar_rect(), wm.mx, wm.my)) {
        shell_mouse(EV_MOUSE_DOWN, wm.mx, wm.my, btn, click_count, 0);
        return;
    }
    if (!w) {
        shell_mouse(EV_MOUSE_DOWN, wm.mx, wm.my, btn, click_count, 0);
        return;
    }
    if (!(w->flags & WF_POPUP)) {
        close_popups_except(0);
        win_raise(w);
    }
    win_focus(w);
    switch (part) {
    case PART_CLOSE: case PART_MAX: case PART_MIN:
        if (btn == MOUSE_LEFT) {
            pressed_win = w;
            pressed_part = part;
            damage_win(w);
        }
        break;
    case PART_TITLE:
        if (btn != MOUSE_LEFT) break;
        if (click_count == 2 && (w->flags & WF_RESIZABLE)) {
            win_toggle_maximize(w);
            click_count = 0;
            break;
        }
        drag.mode = DR_MOVE;
        drag.win = w;
        drag.ox = wm.mx - w->x;
        drag.oy = wm.my - w->y;
        drag.smx = wm.mx;
        drag.smy = wm.my;
        drag.snap = 0;
        drag.moved = false;
        break;
    case PART_EDGE:
        if (btn != MOUSE_LEFT) break;
        drag.mode = DR_RESIZE;
        drag.win = w;
        drag.edges = edges;
        drag.start = win_frame(w);
        drag.smx = wm.mx;
        drag.smy = wm.my;
        break;
    case PART_CONTENT:
        drag.mode = DR_CAPTURE;
        drag.win = w;
        mouse_event_to(w, EV_MOUSE_DOWN, btn, click_count, 0);
        break;
    }
}

static void button_up(uint32_t btn) {
    if (drag.mode == DR_CAPTURE && drag.win) {
        mouse_event_to(drag.win, EV_MOUSE_UP, btn, 0, 0);
        if (!wm.buttons) { drag.mode = DR_NONE; drag.win = 0; }
        update_hover();
        return;
    }
    if (drag.mode == DR_MOVE && btn == MOUSE_LEFT) {
        window_t *w = drag.win;
        int snap = drag.snap;
        drag.mode = DR_NONE;
        drag.win = 0;
        drag.snap = 0;
        wm_damage(wm_work_area());
        if (w && snap) win_snap(w, snap);
        shell_windows_changed();
        update_hover();
        return;
    }
    if (drag.mode == DR_RESIZE && btn == MOUSE_LEFT) {
        drag.mode = DR_NONE;
        drag.win = 0;
        update_hover();
        return;
    }
    if (pressed_win && btn == MOUSE_LEFT) {
        window_t *w = pressed_win;
        int p = pressed_part;
        pressed_win = 0;
        pressed_part = 0;
        damage_win(w);
        int part, edges;
        window_t *h = window_at(wm.mx, wm.my, &part, &edges);
        if (h == w && part == p) {
            if (p == PART_CLOSE) win_close_request(w);
            else if (p == PART_MAX) win_toggle_maximize(w);
            else if (p == PART_MIN) win_minimize(w);
        }
        return;
    }
    shell_mouse(EV_MOUSE_UP, wm.mx, wm.my, btn, 0, 0);
}

static void handle_mouse(input_event_t *e) {
    int nx = wm.mx, ny = wm.my;
    if (e->type == IN_MOUSE_ABS) {
        nx = (int)((int64_t)e->dx * (wm.w - 1) / 65535);
        ny = (int)((int64_t)e->dy * (wm.h - 1) / 65535);
    } else {
        nx += e->dx;
        ny += e->dy;
    }
    nx = MAX(0, MIN(nx, wm.w - 1));
    ny = MAX(0, MIN(ny, wm.h - 1));
    if (nx != wm.mx || ny != wm.my) {
        wm_damage(cursor_rect());
        wm.mx = nx;
        wm.my = ny;
        wm_damage(cursor_rect());
        mouse_moved();
    }
    if (e->wheel) {
        if (!shell_mouse(EV_MOUSE_WHEEL, wm.mx, wm.my, 0, 0, e->wheel)) {
            window_t *target = drag.mode == DR_CAPTURE ? drag.win : mouse_over;
            if (!target) {
                int part, edges;
                window_t *w = window_at(wm.mx, wm.my, &part, &edges);
                if (w && part == PART_CONTENT) target = w;
            }
            if (target) mouse_event_to(target, EV_MOUSE_WHEEL, 0, 0, e->wheel);
        }
    }
    uint32_t changed = e->buttons ^ wm.buttons;
    for (uint32_t b = 1; b <= 4; b <<= 1) {
        if (!(changed & b)) continue;
        if (e->buttons & b) { wm.buttons |= b; button_down(b); }
        else { wm.buttons &= ~b; button_up(b); }
    }
}

/* ------------------------------------------------------------------ keyboard */

static void take_screenshot(void) {
    size_t sz = bmp_encoded_size(wm.w, wm.h);
    uint8_t *buf = vmalloc(sz);
    if (!buf) return;
    bmp_encode(wm.scr.px, wm.w, wm.h, buf);
    char path[64];
    for (int i = 1; i < 1000; i++) {
        snprintf(path, sizeof(path), "/home/Screenshot-%d.bmp", i);
        kstat_t st;
        if (vfs_stat(path, &st) < 0) break;
    }
    int r = vfs_write_all(path, buf, sz);
    vfree(buf);
    if (r == 0) wm_notify("Screenshot saved", path + 6, "viewer");
}

static void handle_key(input_event_t *e) {
    uint32_t kc = e->keycode, m = e->mods;
    bool down = e->pressed;
    if (down && kc != KEY_LSUPER && kc != KEY_RSUPER) super_alone = false;
    if ((kc == KEY_LALT) && !down) shell_alt_release();
    if (down) {
        if ((m & MOD_ALT) && kc == KEY_TAB) { shell_alt_tab(!(m & MOD_SHIFT)); return; }
        if ((m & MOD_ALT) && kc == KEY_F4) { if (wm.focused) win_close_request(wm.focused); return; }
        if (kc == KEY_LSUPER || kc == KEY_RSUPER) { super_alone = true; return; }
        if ((m & MOD_CTRL) && kc == KEY_ESC) { shell_key(0xFFFF, 0, 0, true); return; }
        if ((m & MOD_CTRL) && (m & MOD_ALT) && kc == KEY_T) { wm_launch("/bin/terminal", 0); return; }
        if ((m & MOD_CTRL) && (m & MOD_ALT) && kc == KEY_DELETE) { wm_launch("/bin/taskmgr", 0); return; }
        if ((m & MOD_CTRL) && (m & MOD_SHIFT) && kc == KEY_ESC) { wm_launch("/bin/taskmgr", 0); return; }
        if (kc == KEY_PRINT) { take_screenshot(); return; }
        if (m & MOD_SUPER) {
            window_t *f = wm.focused;
            if (kc == KEY_E) { wm_launch("/bin/files", 0); return; }
            if (kc == KEY_D) {
                for (window_t *w = wm.windows; w; w = w->next)
                    if (w->visible && !(w->flags & WF_NO_TASKBAR)) win_minimize(w);
                return;
            }
            if (kc == KEY_LEFT && f) { win_snap(f, 1); return; }
            if (kc == KEY_RIGHT && f) { win_snap(f, 2); return; }
            if (kc == KEY_UP && f) { win_snap(f, 3); return; }
            if (kc == KEY_DOWN && f) {
                if (f->maximized || f->snapped) win_toggle_maximize(f); else win_minimize(f);
                return;
            }
        }
    } else if ((kc == KEY_LSUPER || kc == KEY_RSUPER) && super_alone) {
        super_alone = false;
        shell_key(0xFFFF, 0, 0, true);   /* toggle start menu */
        return;
    }
    if (shell_key(kc, e->ch, m, down)) return;
    window_t *target = wm.focused;
    if (!target || !target->visible || target->minimized) return;
    gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? EV_KEY_DOWN : EV_KEY_UP;
    ev.key = kc;
    ev.ch = e->ch;
    ev.mods = m;
    wm_send(target, &ev);
}

/* ------------------------------------------------------------------ misc services */

typedef struct { char title[64]; char text[128]; char icon[32]; } notify_req_t;

void wm_launch(const char *path, const char *arg) {
    char *argv[3] = { (char *)path, (char *)arg, 0 };
    const char *base = strrchr(path, '/');
    argv[0] = (char *)(base ? base + 1 : path);
    char *envp[] = { "PATH=/bin", "HOME=/home", "USER=user", "TERM=claudeos", 0 };
    int r = proc_spawn(path, argv, envp, 0, "/home", 0, SPAWN_DETACH);
    if (r < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Could not start %s (error %d)", path, r);
        wm_notify("Error", msg, "about");
    }
}

/* open a file with the matching application */
void wm_open_file(const char *path) {
    kstat_t st;
    if (vfs_stat(path, &st) < 0) return;
    if (st.type == FT_DIR) { wm_launch("/bin/files", path); return; }
    const char *dot = strrchr(path, '.');
    const char *ext = dot ? dot + 1 : "";
    if (!strcasecmp(ext, "bmp") || !strcasecmp(ext, "icn")) wm_launch("/bin/viewer", path);
    else if (!strcasecmp(ext, "wav")) wm_launch("/bin/music", path);
    else if (!strncmp(path, "/bin/", 5)) wm_launch(path, 0);
    else wm_launch("/bin/editor", path);
}

void wm_apply_config(bool rerender_wallpaper) {
    theme_by_name(wm.cfg.theme, &wm.theme);
    theme_apply_config(&wm.theme, &wm.cfg);
    mouse_speed = (int)wm.cfg.mouse_speed;
    if (wm.cfg.kbd_layout[0]) keymap_select(wm.cfg.kbd_layout);
    if (rerender_wallpaper) wallpaper_render();
    shell_desktop_reload();
    /* keep maximized windows fitted to the (possibly moved) work area */
    for (window_t *w = wm.windows; w; w = w->next) {
        if (w->maximized) win_set_frame(w, wm_work_area());
        else if (w->snapped) { int s = w->snapped; w->snapped = 0; win_snap(w, s); }
    }
    for (window_t *w = wm.windows; w; w = w->next) send_simple(w, EV_THEME);
    wm_damage_all();
}

void wm_screen_changed(void) {
    vfree(wm.scr.px);
    wm.w = fb.width;
    wm.h = fb.height;
    uint32_t *bb = vmalloc((size_t)wm.w * wm.h * 4);
    gfx_init(&wm.scr, bb, wm.w, wm.h, wm.w);
    wm.mx = MIN(wm.mx, wm.w - 1);
    wm.my = MIN(wm.my, wm.h - 1);
    wallpaper_render();
    shell_desktop_reload();
    rect_t wa = wm_work_area();
    for (window_t *w = wm.windows; w; w = w->next) {
        if (w->maximized) win_set_frame(w, wa);
        else if (w->snapped) { int s = w->snapped; w->snapped = 0; win_snap(w, s); }
        rect_t f = win_frame(w);
        if (f.x + 80 > wa.x + wa.w) w->x = MAX(wa.x, wa.x + wa.w - f.w);
        if (f.y + 40 > wa.y + wa.h) w->y = MAX(wa.y, wa.y + wa.h - f.h);
        gui_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = EV_SCREEN;
        ev.w = wm.w;
        ev.h = wm.h;
        wm_send(w, &ev);
    }
    wm_damage_all();
}

/* ------------------------------------------------------------------ system calls */

static int place_counter;

SYSCALL_DEF(sys_win_create) {
    SYSCALL_UNUSED_ARGS;
    gui_wincreate_t req;
    if (copy_from_user(&req, (void *)a1, sizeof(req)) < 0) return -EFAULT;
    if (!wm.ready) return -ENODEV;
    req.title[sizeof(req.title) - 1] = 0;
    req.icon[sizeof(req.icon) - 1] = 0;
    if (req.w < 1 || req.h < 1 || req.w > 4096 || req.h > 4096) return -EINVAL;
    window_t *w = kzalloc(sizeof(window_t));
    w->buf = vmalloc((size_t)req.w * req.h * 4);
    if (!w->buf) { kfree(w); return -ENOMEM; }
    mutex_lock(&wm.lock);
    gui_client_t *c = client_of(current);
    w->id = ++wm.next_id;
    w->client = c;
    c->nwindows++;
    strlcpy(w->title, req.title, sizeof(w->title));
    strlcpy(w->icon, req.icon, sizeof(w->icon));
    w->flags = req.flags;
    w->cw = req.w;
    w->ch = req.h;
    w->min_w = req.min_w;
    w->min_h = req.min_h;
    w->parent = req.parent;
    w->cursor = CUR_ARROW;
    for (int i = 0; i < req.w * req.h; i++) w->buf[i] = wm.theme.window_bg;
    rect_t wa = wm_work_area();
    rect_t f = win_frame(w);
    window_t *parent = req.parent ? win_find(req.parent) : 0;
    if (req.flags & WF_POPUP) {
        w->x = req.x;
        w->y = req.y;
        if (w->x + f.w > wm.w) w->x = MAX(0, wm.w - f.w);
        if (w->y + f.h > wm.h) w->y = MAX(0, wm.h - f.h);
    } else if ((req.flags & WF_CENTER) || (parent && req.x < 0)) {
        rect_t ref = parent ? win_frame(parent) : wa;
        w->x = ref.x + (ref.w - f.w) / 2;
        w->y = ref.y + (ref.h - f.h) / 2;
    } else if (req.x >= 0 && req.y >= 0) {
        w->x = req.x;
        w->y = req.y;
    } else {
        int k = place_counter++ % 8;
        w->x = wa.x + 80 + k * 32;
        w->y = wa.y + 50 + k * 28;
        if (w->x + f.w > wa.x + wa.w) w->x = MAX(wa.x, wa.x + (wa.w - f.w) / 2);
        if (w->y + f.h > wa.y + wa.h) w->y = MAX(wa.y, wa.y + (wa.h - f.h) / 2);
    }
    if (!(req.flags & WF_POPUP)) {
        w->x = MAX(wa.x - f.w + 80, MIN(w->x, wa.x + wa.w - 80));
        w->y = MAX(wa.y, MIN(w->y, wa.y + wa.h - 40));
    }
    w->visible = !(req.flags & WF_HIDDEN);
    /* append on top */
    window_t **pp = &wm.windows;
    while (*pp) pp = &(*pp)->next;
    *pp = w;
    if ((req.flags & WF_MAXIMIZED) && (req.flags & WF_RESIZABLE)) {
        w->restore = win_frame(w);
        w->maximized = true;
        win_set_frame(w, wa);
    }
    if (w->visible) win_focus(w);
    damage_win(w);
    shell_windows_changed();
    int id = w->id;
    mutex_unlock(&wm.lock);
    input_kick();
    return id;
}

SYSCALL_DEF(sys_win_destroy) {
    SYSCALL_UNUSED_ARGS;
    mutex_lock(&wm.lock);
    window_t *w = own_window((int)a1);
    if (w) win_destroy(w);
    mutex_unlock(&wm.lock);
    input_kick();
    return w ? 0 : -EINVAL;
}

/* win_update(id, pixels, stride, (x<<32)|y, (w<<32)|h) */
SYSCALL_DEF(sys_win_update) {
    SYSCALL_UNUSED_ARGS;
    const uint32_t *px = (const uint32_t *)a2;
    int64_t stride = (int64_t)a3;
    int x = (int)(a4 >> 32), y = (int)(a4 & 0xFFFFFFFF);
    int rw = (int)(a5 >> 32), rh = (int)(a5 & 0xFFFFFFFF);
    mutex_lock(&wm.lock);
    window_t *w = own_window((int)a1);
    if (!w) { mutex_unlock(&wm.lock); return -EINVAL; }
    rect_t r;
    if (!rect_intersect(mkrect(x, y, rw, rh), mkrect(0, 0, MIN(w->cw, (int)stride), w->ch), &r)) {
        mutex_unlock(&wm.lock);
        return 0;
    }
    const uint32_t *first = px + (int64_t)r.y * stride + r.x;
    const uint32_t *last = px + (int64_t)(r.y + r.h - 1) * stride + r.x + r.w;
    if (!user_range_ok(first, (size_t)((const uint8_t *)last - (const uint8_t *)first), false)) {
        mutex_unlock(&wm.lock);
        return -EFAULT;
    }
    for (int j = 0; j < r.h; j++)
        memcpy(&w->buf[(r.y + j) * w->cw + r.x], px + (int64_t)(r.y + j) * stride + r.x, r.w * 4);
    if (w->visible && !w->minimized) {
        rect_t c = win_content(w);
        rect_t sr = mkrect(c.x + r.x, c.y + r.y, r.w, r.h);
        /* rounded corners and alpha windows need their surroundings redrawn */
        if (w->flags & WF_ALPHA) sr = win_bounds(w);
        wm_damage(sr);
    }
    mutex_unlock(&wm.lock);
    input_kick();
    return 0;
}

SYSCALL_DEF(sys_win_set_title) {
    SYSCALL_UNUSED_ARGS;
    char title[64];
    long r = strncpy_from_user(title, (const char *)a2, sizeof(title));
    if (r < 0) {
        if (r != -ENAMETOOLONG) return r;
        if (copy_from_user(title, (const void *)a2, sizeof(title) - 1) < 0) return -EFAULT;
        title[sizeof(title) - 1] = 0;
    }
    mutex_lock(&wm.lock);
    window_t *w = own_window((int)a1);
    if (w) {
        strlcpy(w->title, title, sizeof(w->title));
        damage_win(w);
        shell_windows_changed();
    }
    mutex_unlock(&wm.lock);
    input_kick();
    return w ? 0 : -EINVAL;
}

SYSCALL_DEF(sys_win_resize) {
    SYSCALL_UNUSED_ARGS;
    mutex_lock(&wm.lock);
    window_t *w = own_window((int)a1);
    if (w) {
        w->maximized = false;
        w->snapped = 0;
        int ow = w->cw, oh = w->ch;
        win_set_size(w, (int)a2, (int)a3);
        if (w->flags & WF_POPUP) {
            /* keep popups on screen */
            rect_t f = win_frame(w);
            if (f.x + f.w > wm.w) w->x = MAX(0, wm.w - f.w);
            if (f.y + f.h > wm.h) w->y = MAX(0, wm.h - f.h);
        }
        UNUSED(ow); UNUSED(oh);
    }
    mutex_unlock(&wm.lock);
    input_kick();
    return w ? 0 : -EINVAL;
}

SYSCALL_DEF(sys_win_move) {
    SYSCALL_UNUSED_ARGS;
    mutex_lock(&wm.lock);
    window_t *w = own_window((int)a1);
    if (w) {
        damage_win(w);
        w->x = (int)a2;
        w->y = (int)a3;
        damage_win(w);
    }
    mutex_unlock(&wm.lock);
    input_kick();
    return w ? 0 : -EINVAL;
}

SYSCALL_DEF(sys_win_get_rect) {
    SYSCALL_UNUSED_ARGS;
    int out[8];
    mutex_lock(&wm.lock);
    window_t *w = own_window((int)a1);
    if (w) {
        rect_t f = win_frame(w), c = win_content(w);
        out[0] = f.x; out[1] = f.y; out[2] = f.w; out[3] = f.h;
        out[4] = c.x; out[5] = c.y; out[6] = c.w; out[7] = c.h;
    }
    mutex_unlock(&wm.lock);
    if (!w) return -EINVAL;
    return copy_to_user((void *)a2, out, sizeof(out));
}

SYSCALL_DEF(sys_win_action) {
    SYSCALL_UNUSED_ARGS;
    mutex_lock(&wm.lock);
    window_t *w = own_window((int)a1);
    if (w) {
        switch (a2) {
        case WA_MINIMIZE: win_minimize(w); break;
        case WA_MAXIMIZE: if (!w->maximized) win_toggle_maximize(w); break;
        case WA_RESTORE:
            if (w->maximized || w->snapped) win_toggle_maximize(w);
            win_restore(w);
            break;
        case WA_TOGGLE_MAXIMIZE: win_toggle_maximize(w); break;
        case WA_FOCUS: win_restore(w); break;
        case WA_RAISE: win_raise(w); break;
        case WA_SHOW:
            if (!w->visible) {
                w->visible = true;
                if (w->flags & WF_POPUP) win_raise(w);
                win_focus(w);
                damage_win(w);
                shell_windows_changed();
            }
            break;
        case WA_HIDE:
            if (w->visible) {
                damage_win(w);
                w->visible = false;
                if (wm.focused == w) {
                    wm.focused = 0;
                    window_t *p = w->parent ? win_find(w->parent) : top_visible();
                    if (p) win_focus(p);
                }
                shell_windows_changed();
            }
            break;
        }
    }
    mutex_unlock(&wm.lock);
    input_kick();
    return w ? 0 : -EINVAL;
}

SYSCALL_DEF(sys_win_set_cursor) {
    SYSCALL_UNUSED_ARGS;
    mutex_lock(&wm.lock);
    window_t *w = own_window((int)a1);
    if (w && a2 < CUR_COUNT) {
        w->cursor = (int)a2;
        if (mouse_over == w || (drag.mode == DR_CAPTURE && drag.win == w)) set_cursor(w->cursor);
    }
    mutex_unlock(&wm.lock);
    input_kick();
    return w ? 0 : -EINVAL;
}

/* gui_event(ev*, timeout_ms): 1 = event, 0 = timeout */
SYSCALL_DEF(sys_gui_event) {
    SYSCALL_UNUSED_ARGS;
    if (!user_range_ok((void *)a1, sizeof(gui_event_t), true)) return -EFAULT;
    gui_client_t *c = current->gui;
    if (!c) {
        if ((int64_t)a2 > 0) sleep_ms(a2);
        return 0;
    }
    int64_t timeout = (int64_t)a2;
    uint64_t deadline = timeout < 0 ? ~0ULL : uptime_ms() + (uint64_t)timeout;
    for (;;) {
        uint64_t f = irq_save();
        if (c->head != c->tail) {
            gui_event_t ev = c->q[c->tail];
            c->tail = (c->tail + 1) % EVQ_SIZE;
            irq_restore(f);
            memcpy((void *)a1, &ev, sizeof(ev));
            return 1;
        }
        uint64_t now = uptime_ms();
        if (now >= deadline || current->killed) { irq_restore(f); return 0; }
        wq_wait_timeout(&c->wq, MIN(deadline - now, 1000));
        irq_restore(f);
    }
}

SYSCALL_DEF(sys_screen_info) {
    SYSCALL_UNUSED_ARGS;
    gui_screen_t si;
    memset(&si, 0, sizeof(si));
    mutex_lock(&wm.lock);
    si.width = wm.w;
    si.height = wm.h;
    rect_t wa = wm_work_area();
    si.work_x = wa.x; si.work_y = wa.y; si.work_w = wa.w; si.work_h = wa.h;
    si.can_set_mode = fb_can_set_mode();
    mutex_unlock(&wm.lock);
    return copy_to_user((void *)a1, &si, sizeof(si));
}

SYSCALL_DEF(sys_theme_get) {
    SYSCALL_UNUSED_ARGS;
    if (a2) {
        /* theme_get(out, index): fetch a preset by index (for the settings app) */
        if ((int64_t)a2 - 1 >= theme_count()) return -EINVAL;
        gui_theme_t t;
        theme_by_name(theme_name((int)a2 - 1), &t);
        return copy_to_user((void *)a1, &t, sizeof(t));
    }
    return copy_to_user((void *)a1, &wm.theme, sizeof(gui_theme_t));
}

SYSCALL_DEF(sys_wm_config_get) {
    SYSCALL_UNUSED_ARGS;
    return copy_to_user((void *)a1, &wm.cfg, sizeof(gui_config_t));
}

SYSCALL_DEF(sys_wm_config_set) {
    SYSCALL_UNUSED_ARGS;
    gui_config_t c;
    if (copy_from_user(&c, (void *)a1, sizeof(c)) < 0) return -EFAULT;
    c.theme[sizeof(c.theme) - 1] = 0;
    c.wp_image[sizeof(c.wp_image) - 1] = 0;
    c.kbd_layout[sizeof(c.kbd_layout) - 1] = 0;
    if (c.mouse_speed < 1 || c.mouse_speed > 10) c.mouse_speed = 5;
    if (c.volume > 100) c.volume = 100;
    if (c.wallpaper >= WP_COUNT) c.wallpaper = WP_WAVES;
    if (c.double_click_ms < 150 || c.double_click_ms > 1500) c.double_click_ms = 450;
    mutex_lock(&wm.lock);
    bool wp = c.wallpaper != wm.cfg.wallpaper || c.wp_color1 != wm.cfg.wp_color1 ||
              c.wp_color2 != wm.cfg.wp_color2 || strcmp(c.wp_image, wm.cfg.wp_image) || c.wp_mode != wm.cfg.wp_mode;
    wm.cfg = c;
    wm_apply_config(wp);
    mutex_unlock(&wm.lock);
    extern void snd_set_volume(int v);
    snd_set_volume((int)c.volume);
    if (!a2) config_save(&c);   /* a2 != 0: preview only, do not persist */
    input_kick();
    return 0;
}

SYSCALL_DEF(sys_clipboard_set) {
    SYSCALL_UNUSED_ARGS;
    size_t n = MIN(a2, (size_t)1 << 20);
    if (!user_range_ok((void *)a1, n, false)) return -EFAULT;
    char *buf = kmalloc(n + 1);
    memcpy(buf, (void *)a1, n);
    buf[n] = 0;
    mutex_lock(&wm.lock);
    kfree(clipboard);
    clipboard = buf;
    clipboard_len = n;
    mutex_unlock(&wm.lock);
    return 0;
}

SYSCALL_DEF(sys_clipboard_get) {
    SYSCALL_UNUSED_ARGS;
    mutex_lock(&wm.lock);
    size_t n = MIN(clipboard_len, a2);
    long r = (long)clipboard_len;
    if (a1 && n) {
        if (!user_range_ok((void *)a1, n, true)) r = -EFAULT;
        else memcpy((void *)a1, clipboard, n);
    }
    mutex_unlock(&wm.lock);
    return r;
}

SYSCALL_DEF(sys_notify) {
    SYSCALL_UNUSED_ARGS;
    char title[64], text[128], icon[32] = "about";
    if (strncpy_from_user(title, (const char *)a1, sizeof(title)) < 0) return -EFAULT;
    if (strncpy_from_user(text, (const char *)a2, sizeof(text)) < 0) return -EFAULT;
    if (a3 && strncpy_from_user(icon, (const char *)a3, sizeof(icon)) < 0) return -EFAULT;
    mutex_lock(&wm.lock);
    wm_notify(title, text, icon);
    mutex_unlock(&wm.lock);
    input_kick();
    return 0;
}

SYSCALL_DEF(sys_set_resolution) {
    SYSCALL_UNUSED_ARGS;
    if (!fb_can_set_mode()) return -ENODEV;
    mutex_lock(&wm.lock);
    bool ok = fb_set_mode((uint32_t)a1, (uint32_t)a2);
    if (ok) wm_screen_changed();
    mutex_unlock(&wm.lock);
    input_kick();
    return ok ? 0 : -EINVAL;
}

/* launch(path, arg): open a file/program through the desktop (detached) */
SYSCALL_DEF(sys_launch) {
    SYSCALL_UNUSED_ARGS;
    char path[PATH_MAX_LEN], abs[PATH_MAX_LEN], arg[PATH_MAX_LEN];
    if (strncpy_from_user(path, (const char *)a1, sizeof(path)) < 0) return -EFAULT;
    if (vfs_normalize(current->cwd, path, abs) < 0) return -EINVAL;
    if (a2) {
        if (strncpy_from_user(arg, (const char *)a2, sizeof(arg)) < 0) return -EFAULT;
        char absarg[PATH_MAX_LEN];
        if (arg[0] && arg[0] != '-' && vfs_normalize(current->cwd, arg, absarg) == 0) {
            kstat_t st;
            if (vfs_stat(absarg, &st) == 0) strlcpy(arg, absarg, sizeof(arg));
        }
        wm_launch(abs, arg);
    } else {
        wm_open_file(abs);
    }
    return 0;
}

/* ------------------------------------------------------------------ thread */

static int wm_thread(void *arg) {
    UNUSED(arg);
    for (;;) {
        input_wait(40);
        mutex_lock(&wm.lock);
        input_event_t e;
        int n = 0;
        while (n < 256 && input_pop(&e)) {
            if (e.type == IN_KEY) handle_key(&e);
            else handle_mouse(&e);
            n++;
        }
        shell_tick();
        compose();
        mutex_unlock(&wm.lock);
    }
    return 0;
}

void wm_init(void) {
    if (!fb.present) {
        klog("[wm] no framebuffer, desktop disabled\n");
        return;
    }
    wm.w = fb.width;
    wm.h = fb.height;
    uint32_t *bb = vmalloc((size_t)wm.w * wm.h * 4);
    gfx_init(&wm.scr, bb, wm.w, wm.h, wm.w);
    config_load(&wm.cfg);
    theme_by_name(wm.cfg.theme, &wm.theme);
    theme_apply_config(&wm.theme, &wm.cfg);
    mouse_speed = (int)wm.cfg.mouse_speed;
    if (wm.cfg.kbd_layout[0]) keymap_select(wm.cfg.kbd_layout);
    wm.mx = wm.w / 2;
    wm.my = wm.h / 2;
    wallpaper_render();
    shell_init();
    syscall_register(SYS_WIN_CREATE, sys_win_create);
    syscall_register(SYS_WIN_DESTROY, sys_win_destroy);
    syscall_register(SYS_WIN_UPDATE, sys_win_update);
    syscall_register(SYS_WIN_SET_TITLE, sys_win_set_title);
    syscall_register(SYS_WIN_RESIZE, sys_win_resize);
    syscall_register(SYS_WIN_MOVE, sys_win_move);
    syscall_register(SYS_WIN_GET_RECT, sys_win_get_rect);
    syscall_register(SYS_WIN_ACTION, sys_win_action);
    syscall_register(SYS_WIN_SET_CURSOR, sys_win_set_cursor);
    syscall_register(SYS_GUI_EVENT, sys_gui_event);
    syscall_register(SYS_SCREEN_INFO, sys_screen_info);
    syscall_register(SYS_THEME_GET, sys_theme_get);
    syscall_register(SYS_WM_CONFIG_GET, sys_wm_config_get);
    syscall_register(SYS_WM_CONFIG_SET, sys_wm_config_set);
    syscall_register(SYS_CLIPBOARD_SET, sys_clipboard_set);
    syscall_register(SYS_CLIPBOARD_GET, sys_clipboard_get);
    syscall_register(SYS_NOTIFY, sys_notify);
    syscall_register(SYS_SET_RESOLUTION, sys_set_resolution);
    syscall_register(SYS_LAUNCH, sys_launch);
    wm.ready = true;
    extern void snd_set_volume(int v);
    extern void snd_play_file(const char *path);
    snd_set_volume((int)wm.cfg.volume);
    if (wm.cfg.sounds) snd_play_file("/system/sounds/startup.wav");
    bootcon_disable();
    wm_damage_all();
    task_t *t = kthread_create("wm", wm_thread, 0);
    t->prio = 1;
    klog("[wm] desktop ready (%dx%d, theme %s)\n", wm.w, wm.h, wm.theme.name);
}
