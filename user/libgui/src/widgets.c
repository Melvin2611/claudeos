/* libgui widgets */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ui_menubar_event(ui_widget_t *w, gui_event_t *ev);
void ui_menubar_draw(ui_widget_t *w, surface_t *s);

/* ------------------------------------------------------------------ state types */
typedef struct {
    char *buf;
    int cap, len;
    int cursor, anchor;     /* byte offsets; selection is [min,max) of cursor/anchor */
    int scroll;             /* pixels */
    char *placeholder;
    bool password;
    bool dragging;
} textbox_t;

typedef struct {
    char *text;
    char icon[32];
    void *data;
} list_item_t;

typedef struct {
    list_item_t *items;
    int count, cap;
    int scroll;             /* pixels */
    int row_h;
    ui_column_t cols[8];
    int ncols;
    int hover;
    bool grid;
    bool drag_scroll;
} list_t;

typedef struct {
    char **items;
    int count;
} dropdown_t;

typedef struct { bool pressed; } button_t;

/* ------------------------------------------------------------------ creation */
ui_widget_t *ui_add(ui_window_t *win, int type, int x, int y, int w, int h, const char *text) {
    ui_widget_t *wd = calloc(1, sizeof(ui_widget_t));
    wd->type = type;
    wd->r = mkrect(x, y, w, h);
    wd->anchor = A_LEFT | A_TOP;
    wd->mr = win->w - (x + w);
    wd->mb = win->h - (y + h);
    wd->visible = true;
    wd->enabled = true;
    wd->text = strdup(text ? text : "");
    wd->font = &ui_font;
    wd->win = win;
    /* append */
    ui_widget_t **pp = &win->widgets;
    while (*pp) pp = &(*pp)->next;
    *pp = wd;
    ui_widget_invalidate(wd);
    return wd;
}

void ui_widget_free(ui_widget_t *w) {
    if (w->type == W_TEXTBOX) {
        textbox_t *t = w->data;
        free(t->buf);
        free(t->placeholder);
        free(t);
    } else if (w->type == W_LIST) {
        list_t *l = w->data;
        for (int i = 0; i < l->count; i++) free(l->items[i].text);
        free(l->items);
        free(l);
    } else if (w->type == W_DROPDOWN) {
        dropdown_t *d = w->data;
        for (int i = 0; i < d->count; i++) free(d->items[i]);
        free(d->items);
        free(d);
    } else if (w->type == W_BUTTON || w->type == W_TOOLBUTTON) {
        free(w->data);
    }
    free(w->text);
    free(w);
}

void ui_set_anchor(ui_widget_t *w, int a) {
    w->anchor = a;
    w->mr = w->win->w - (w->r.x + w->r.w);
    w->mb = w->win->h - (w->r.y + w->r.h);
}

void ui_show(ui_widget_t *w, bool v) { if (w->visible != v) { w->visible = v; ui_invalidate_rect(w->win, w->r); } }
void ui_enable(ui_widget_t *w, bool e) { if (w->enabled != e) { w->enabled = e; ui_widget_invalidate(w); } }

ui_widget_t *ui_label(ui_window_t *win, int x, int y, int w, int h, const char *text) {
    return ui_add(win, W_LABEL, x, y, w, h, text);
}

ui_widget_t *ui_button(ui_window_t *win, int x, int y, int w, int h, const char *text, ui_cb cb) {
    ui_widget_t *b = ui_add(win, W_BUTTON, x, y, w, h, text);
    b->on_click = cb;
    b->focusable = true;
    b->data = calloc(1, sizeof(button_t));
    return b;
}

ui_widget_t *ui_toolbutton(ui_window_t *win, int x, int y, int size, const char *glyph, const char *tip, ui_cb cb) {
    ui_widget_t *b = ui_add(win, W_TOOLBUTTON, x, y, size, size, tip);
    strlcpy(b->glyph, glyph, sizeof(b->glyph));
    b->on_click = cb;
    b->data = calloc(1, sizeof(button_t));
    return b;
}

void ui_button_style(ui_widget_t *w, int style) { w->ival = style; ui_widget_invalidate(w); }

ui_widget_t *ui_textbox(ui_window_t *win, int x, int y, int w, int h, const char *text) {
    ui_widget_t *t = ui_add(win, W_TEXTBOX, x, y, w, h, "");
    textbox_t *tb = calloc(1, sizeof(textbox_t));
    tb->cap = 256;
    tb->buf = malloc(tb->cap);
    tb->buf[0] = 0;
    t->data = tb;
    t->focusable = true;
    ui_set_text(t, text);
    return t;
}

ui_widget_t *ui_checkbox(ui_window_t *win, int x, int y, int w, int h, const char *text, bool checked) {
    ui_widget_t *c = ui_add(win, W_CHECKBOX, x, y, w, h, text);
    c->ival = checked;
    c->focusable = true;
    return c;
}

ui_widget_t *ui_slider(ui_window_t *win, int x, int y, int w, int h, int min, int max, int value) {
    ui_widget_t *s = ui_add(win, W_SLIDER, x, y, w, h, "");
    s->imin = min;
    s->imax = max;
    s->ival = value;
    s->focusable = true;
    return s;
}

ui_widget_t *ui_list(ui_window_t *win, int x, int y, int w, int h) {
    ui_widget_t *l = ui_add(win, W_LIST, x, y, w, h, "");
    list_t *ls = calloc(1, sizeof(list_t));
    ls->row_h = 26;
    ls->hover = -1;
    l->data = ls;
    l->ival = -1;
    l->focusable = true;
    return l;
}

ui_widget_t *ui_progress(ui_window_t *win, int x, int y, int w, int h) {
    ui_widget_t *p = ui_add(win, W_PROGRESS, x, y, w, h, "");
    p->imax = 100;
    return p;
}

ui_widget_t *ui_dropdown(ui_window_t *win, int x, int y, int w, int h) {
    ui_widget_t *d = ui_add(win, W_DROPDOWN, x, y, w, h, "");
    d->data = calloc(1, sizeof(dropdown_t));
    d->ival = -1;
    d->focusable = true;
    return d;
}

ui_widget_t *ui_canvas(ui_window_t *win, int x, int y, int w, int h, void (*draw)(ui_widget_t *, surface_t *),
                       bool (*event)(ui_widget_t *, gui_event_t *)) {
    ui_widget_t *c = ui_add(win, W_CANVAS, x, y, w, h, "");
    c->draw = draw;
    c->event = event;
    c->focusable = true;
    return c;
}

ui_widget_t *ui_panel(ui_window_t *win, int x, int y, int w, int h, uint32_t color) {
    ui_widget_t *p = ui_add(win, W_PANEL, x, y, w, h, "");
    p->color = color;
    return p;
}

ui_widget_t *ui_swatch(ui_window_t *win, int x, int y, int w, int h, uint32_t color, ui_cb cb) {
    ui_widget_t *s = ui_add(win, W_SWATCH, x, y, w, h, "");
    s->color = color;
    s->on_click = cb;
    return s;
}

/* ------------------------------------------------------------------ text */
void ui_set_text(ui_widget_t *w, const char *text) {
    if (!text) text = "";
    if (w->type == W_TEXTBOX) {
        textbox_t *t = w->data;
        int n = (int)strlen(text);
        if (n + 1 > t->cap) { t->cap = n + 64; t->buf = realloc(t->buf, t->cap); }
        memcpy(t->buf, text, n + 1);
        t->len = n;
        t->cursor = t->anchor = n;
        t->scroll = 0;
    } else {
        free(w->text);
        w->text = strdup(text);
    }
    ui_widget_invalidate(w);
}

const char *ui_get_text(ui_widget_t *w) {
    if (w->type == W_TEXTBOX) return ((textbox_t *)w->data)->buf;
    return w->text;
}

void ui_textbox_select_all(ui_widget_t *w) {
    textbox_t *t = w->data;
    t->anchor = 0;
    t->cursor = t->len;
    ui_widget_invalidate(w);
}

void ui_textbox_set_placeholder(ui_widget_t *w, const char *text) {
    textbox_t *t = w->data;
    free(t->placeholder);
    t->placeholder = strdup(text);
}

void ui_textbox_set_password(ui_widget_t *w, bool pw) { ((textbox_t *)w->data)->password = pw; }

/* ------------------------------------------------------------------ list API */
void ui_list_clear(ui_widget_t *w) {
    list_t *l = w->data;
    for (int i = 0; i < l->count; i++) free(l->items[i].text);
    l->count = 0;
    l->scroll = 0;
    w->ival = -1;
    ui_widget_invalidate(w);
}

int ui_list_add(ui_widget_t *w, const char *text, const char *icon, void *data) {
    list_t *l = w->data;
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = realloc(l->items, sizeof(list_item_t) * l->cap);
    }
    list_item_t *it = &l->items[l->count];
    it->text = strdup(text);
    it->icon[0] = 0;
    if (icon) strlcpy(it->icon, icon, sizeof(it->icon));
    it->data = data;
    ui_widget_invalidate(w);
    return l->count++;
}

void ui_list_set_columns(ui_widget_t *w, const ui_column_t *cols, int n) {
    list_t *l = w->data;
    l->ncols = MIN(n, 8);
    memcpy(l->cols, cols, sizeof(ui_column_t) * l->ncols);
}

int ui_list_count(ui_widget_t *w) { return ((list_t *)w->data)->count; }
const char *ui_list_text(ui_widget_t *w, int i) {
    list_t *l = w->data;
    return i >= 0 && i < l->count ? l->items[i].text : 0;
}
void *ui_list_data(ui_widget_t *w, int i) {
    list_t *l = w->data;
    return i >= 0 && i < l->count ? l->items[i].data : 0;
}
void ui_list_set_text(ui_widget_t *w, int i, const char *text) {
    list_t *l = w->data;
    if (i < 0 || i >= l->count) return;
    free(l->items[i].text);
    l->items[i].text = strdup(text);
    ui_widget_invalidate(w);
}
void ui_list_set_row_height(ui_widget_t *w, int h) { ((list_t *)w->data)->row_h = h; }
void ui_list_set_icon_mode(ui_widget_t *w, bool grid) { ((list_t *)w->data)->grid = grid; ui_widget_invalidate(w); }

#define TILE_W 100
#define TILE_H 92

static int list_header_h(list_t *l) { return l->ncols ? 26 : 0; }

static int list_cols_per_row(ui_widget_t *w) { return MAX(1, (w->r.w - 12) / TILE_W); }

static int list_content_h(ui_widget_t *w) {
    list_t *l = w->data;
    if (l->grid) return ((l->count + list_cols_per_row(w) - 1) / list_cols_per_row(w)) * TILE_H + 8;
    return l->count * l->row_h;
}

static int list_view_h(ui_widget_t *w) { return w->r.h - list_header_h(w->data) - 2; }

static void list_clamp_scroll(ui_widget_t *w) {
    list_t *l = w->data;
    int maxs = MAX(0, list_content_h(w) - list_view_h(w));
    l->scroll = MAX(0, MIN(l->scroll, maxs));
}

static rect_t list_item_rect(ui_widget_t *w, int i) {
    list_t *l = w->data;
    int top = w->r.y + 1 + list_header_h(l);
    if (l->grid) {
        int cols = list_cols_per_row(w);
        int gx = w->r.x + 6 + ((w->r.w - 12) - cols * TILE_W) / 2;
        return mkrect(gx + (i % cols) * TILE_W, top + 4 + (i / cols) * TILE_H - l->scroll, TILE_W - 4, TILE_H - 4);
    }
    return mkrect(w->r.x + 1, top + i * l->row_h - l->scroll, w->r.w - 2, l->row_h);
}

static void list_ensure_visible(ui_widget_t *w, int i) {
    list_t *l = w->data;
    rect_t r = list_item_rect(w, i);
    int top = w->r.y + 1 + list_header_h(l);
    int bottom = top + list_view_h(w);
    if (r.y < top) l->scroll -= top - r.y;
    else if (r.y + r.h > bottom) l->scroll += r.y + r.h - bottom;
    list_clamp_scroll(w);
}

void ui_list_select(ui_widget_t *w, int i) {
    list_t *l = w->data;
    if (i >= l->count) i = l->count - 1;
    w->ival = i;
    if (i >= 0) list_ensure_visible(w, i);
    ui_widget_invalidate(w);
}

static int list_index_at(ui_widget_t *w, int x, int y) {
    list_t *l = w->data;
    int top = w->r.y + 1 + list_header_h(l);
    if (y < top) return -1;
    for (int i = 0; i < l->count; i++) {
        rect_t r = list_item_rect(w, i);
        if (r.y > y) break;
        if (rect_contains(r, x, y)) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ dropdown API */
void ui_dropdown_add(ui_widget_t *w, const char *text) {
    dropdown_t *d = w->data;
    d->items = realloc(d->items, sizeof(char *) * (d->count + 1));
    d->items[d->count++] = strdup(text);
    if (w->ival < 0) w->ival = 0;
    ui_widget_invalidate(w);
}

void ui_dropdown_clear(ui_widget_t *w) {
    dropdown_t *d = w->data;
    for (int i = 0; i < d->count; i++) free(d->items[i]);
    d->count = 0;
    w->ival = -1;
    ui_widget_invalidate(w);
}

void ui_dropdown_select(ui_widget_t *w, int i) {
    dropdown_t *d = w->data;
    if (i >= 0 && i < d->count) w->ival = i;
    ui_widget_invalidate(w);
}

/* ------------------------------------------------------------------ drawing */
static void draw_focus(surface_t *s, ui_widget_t *w) {
    if (w->win->focus != w || !w->win->focused) return;
    if (ui_is_retro()) {
        for (int x = w->r.x + 3; x < w->r.x + w->r.w - 3; x += 2) {
            gfx_pixel(s, x, w->r.y + 3, 0xFF000000);
            gfx_pixel(s, x, w->r.y + w->r.h - 4, 0xFF000000);
        }
        return;
    }
    gfx_rounded_rect(s, w->r.x - 1, w->r.y - 1, w->r.w + 2, w->r.h + 2, 6, WITH_ALPHA(ui_theme.accent, 160));
}

static void draw_textbox(ui_widget_t *w, surface_t *s) {
    textbox_t *t = w->data;
    bool focused = w->win->focus == w && w->win->focused;
    ui_draw_input_frame(s, w->r, focused);
    rect_t inner = mkrect(w->r.x + 7, w->r.y + 2, w->r.w - 14, w->r.h - 4);
    surface_t sub = *s;
    rect_t cl;
    if (!rect_intersect(s->clip, inner, &cl)) return;
    gfx_set_clip(&sub, cl);
    const font_t *f = w->font;
    int ty = w->r.y + (w->r.h - f->height) / 2;
    const char *text = t->buf;
    char *masked = 0;
    if (t->password) {
        int n = utf8_len(t->buf);
        masked = malloc(n * 3 + 1);
        for (int i = 0; i < n; i++) memcpy(masked + i * 3, "\xE2\x80\xA2", 3);
        masked[n * 3] = 0;
        text = masked;
    }
    /* map byte offsets for password text */
    int cur = t->cursor, anc = t->anchor;
    if (t->password) {
        int cc = 0, ac = 0;
        for (int i = 0; i < t->cursor; i = utf8_next(t->buf, i)) cc++;
        for (int i = 0; i < t->anchor; i = utf8_next(t->buf, i)) ac++;
        cur = cc * 3;
        anc = ac * 3;
    }
    int cx = font_text_width_n(f, text, cur);
    /* keep cursor visible */
    if (cx - t->scroll > inner.w - 2) t->scroll = cx - inner.w + 2;
    if (cx - t->scroll < 0) t->scroll = cx;
    int x0 = inner.x - t->scroll;
    if (cur != anc && focused) {
        int a = MIN(cur, anc), b = MAX(cur, anc);
        int sx = font_text_width_n(f, text, a), ex = font_text_width_n(f, text, b);
        gfx_fill(&sub, x0 + sx, ty, ex - sx, f->height, WITH_ALPHA(ui_theme.selection_bg, 150));
    }
    if (!t->len && t->placeholder && !focused)
        font_draw(&sub, f, inner.x, ty, t->placeholder, ui_theme.window_text_dim);
    else
        font_draw(&sub, f, x0, ty, text, w->enabled ? ui_theme.input_text : ui_theme.window_text_dim);
    if (focused) gfx_vline(&sub, x0 + cx, ty, f->height, ui_theme.input_text);
    free(masked);
}

static void draw_list(ui_widget_t *w, surface_t *s) {
    list_t *l = w->data;
    bool focused = w->win->focus == w && w->win->focused;
    if (ui_is_retro()) {
        gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, ui_theme.input_bg);
        ui_bevel(s, w->r, true);
    } else {
        gfx_fill_rounded(s, w->r.x, w->r.y, w->r.w, w->r.h, 6, ui_theme.input_bg);
        gfx_rounded_rect(s, w->r.x, w->r.y, w->r.w, w->r.h, 6, focused ? WITH_ALPHA(ui_theme.accent, 180) : ui_theme.input_border);
    }
    int hh = list_header_h(l);
    surface_t sub = *s;
    rect_t cl;
    if (hh) {
        int x = w->r.x + 1;
        gfx_fill(s, w->r.x + 1, w->r.y + 1, w->r.w - 2, hh - 1, ui_theme.sidebar_bg);
        gfx_hline(s, w->r.x + 1, w->r.y + hh, w->r.w - 2, ui_theme.input_border);
        for (int c = 0; c < l->ncols; c++) {
            int cw = l->cols[c].width < 0 ? w->r.x + w->r.w - x - 1 : l->cols[c].width;
            font_draw_fit(s, &ui_font_bold, x + 8, w->r.y + (hh - ui_font.height) / 2, l->cols[c].title, cw - 12,
                          ui_theme.window_text_dim);
            x += cw;
            if (c < l->ncols - 1) gfx_vline(s, x - 1, w->r.y + 5, hh - 10, ui_theme.input_border);
        }
    }
    rect_t view = mkrect(w->r.x + 1, w->r.y + 1 + hh, w->r.w - 2, w->r.h - 2 - hh);
    if (!rect_intersect(s->clip, view, &cl)) return;
    gfx_set_clip(&sub, cl);
    for (int i = 0; i < l->count; i++) {
        rect_t r = list_item_rect(w, i);
        if (r.y + r.h < view.y) continue;
        if (r.y > view.y + view.h) break;
        list_item_t *it = &l->items[i];
        bool sel = i == w->ival;
        uint32_t tc = ui_theme.window_text;
        if (sel) {
            uint32_t bg = focused ? ui_theme.selection_bg : gfx_mix(ui_theme.selection_bg, ui_theme.input_bg, 110);
            if (l->grid) gfx_fill_rounded(&sub, r.x, r.y, r.w, r.h, 6, WITH_ALPHA(bg, 200));
            else gfx_fill(&sub, r.x, r.y, r.w, r.h, bg);
            tc = ui_theme.selection_text;
        } else if (i == l->hover) {
            if (l->grid) gfx_fill_rounded(&sub, r.x, r.y, r.w, r.h, 6, ui_theme.menu_hover);
            else gfx_fill(&sub, r.x, r.y, r.w, r.h, ui_theme.menu_hover);
        }
        if (l->grid) {
            if (it->icon[0]) ui_draw_icon(&sub, it->icon, 32, r.x + (r.w - 32) / 2, r.y + 8);
            /* two-line centered label */
            const char *t = it->text;
            int tw = font_text_width(&ui_font, t);
            if (tw <= r.w - 8) font_draw(&sub, &ui_font, r.x + (r.w - tw) / 2, r.y + 48, t, tc);
            else {
                int n = (int)strlen(t), cut = n;
                while (cut > 0 && font_text_width_n(&ui_font, t, cut) > r.w - 8) cut = utf8_prev(t, cut);
                font_draw_n(&sub, &ui_font, r.x + 4, r.y + 48, t, cut, tc);
                font_draw_fit(&sub, &ui_font, r.x + 4, r.y + 48 + ui_font.height, t + cut, r.w - 8, tc);
            }
            continue;
        }
        int x = r.x + 8;
        if (it->icon[0]) {
            int isz = l->row_h >= 30 ? 24 : 16;
            if (!strncmp(it->icon, "g:", 2)) {
                isz = l->row_h >= 30 ? 20 : 16;
                ui_draw_glyph(&sub, it->icon + 2, isz, x + 2, r.y + (r.h - isz) / 2, tc);
            } else {
                ui_draw_icon(&sub, it->icon, isz, x, r.y + (r.h - isz) / 2);
            }
            x += isz + 8;
        }
        int ty = r.y + (r.h - w->font->height) / 2;
        if (l->ncols) {
            /* tab separated columns */
            const char *p = it->text;
            int colx = r.x;
            for (int c = 0; c < l->ncols && p; c++) {
                const char *tab = strchr(p, '\t');
                int len = tab ? (int)(tab - p) : (int)strlen(p);
                char cell[256];
                len = MIN(len, 255);
                memcpy(cell, p, len);
                cell[len] = 0;
                int cw = l->cols[c].width < 0 ? r.x + r.w - colx : l->cols[c].width;
                int cx = c == 0 ? x : colx + 8;
                font_draw_fit(&sub, w->font, cx, ty, cell, cw - (cx - colx) - 6, c == 0 ? tc : (sel ? tc : ui_theme.window_text_dim));
                colx += cw;
                p = tab ? tab + 1 : 0;
            }
        } else {
            font_draw_fit(&sub, w->font, x, ty, it->text, r.x + r.w - x - 8, tc);
        }
    }
    rect_t track = mkrect(view.x + view.w - 12, view.y, 12, view.h);
    ui_draw_scrollbar(&sub, track, list_content_h(w), list_view_h(w), l->scroll, false);
}

static void draw_checkbox(ui_widget_t *w, surface_t *s) {
    int bs = 18;
    rect_t b = mkrect(w->r.x, w->r.y + (w->r.h - bs) / 2, bs, bs);
    bool hover = w->win->hover == w;
    if (ui_is_retro()) {
        gfx_fill(s, b.x, b.y, bs, bs, 0xFFFFFFFF);
        ui_bevel(s, b, true);
        if (w->ival) {
            gfx_line(s, b.x + 4, b.y + 9, b.x + 7, b.y + 12, 0xFF000000);
            gfx_line(s, b.x + 7, b.y + 12, b.x + 13, b.y + 5, 0xFF000000);
            gfx_line(s, b.x + 4, b.y + 8, b.x + 7, b.y + 11, 0xFF000000);
            gfx_line(s, b.x + 7, b.y + 11, b.x + 13, b.y + 4, 0xFF000000);
        }
    } else if (w->ival) {
        gfx_fill_rounded(s, b.x, b.y, bs, bs, 4, hover ? gfx_lighten(ui_theme.accent, 25) : ui_theme.accent);
        ui_draw_glyph(s, "check", 16, b.x + 1, b.y + 1, ui_theme.accent_text);
    } else {
        gfx_fill_rounded(s, b.x, b.y, bs, bs, 4, hover ? ui_theme.button_hover : ui_theme.input_bg);
        gfx_rounded_rect(s, b.x, b.y, bs, bs, 4, ui_theme.input_border);
    }
    font_draw_fit(s, w->font, w->r.x + bs + 8, w->r.y + (w->r.h - w->font->height) / 2, w->text, w->r.w - bs - 8,
                  w->enabled ? ui_theme.window_text : ui_theme.window_text_dim);
    draw_focus(s, w);
}

static int slider_pos(ui_widget_t *w) {
    int range = MAX(1, w->imax - w->imin);
    return w->r.x + 8 + (w->r.w - 16) * (w->ival - w->imin) / range;
}

static void draw_slider(ui_widget_t *w, surface_t *s) {
    int cy = w->r.y + w->r.h / 2;
    int px = slider_pos(w);
    if (ui_is_retro()) {
        gfx_fill(s, w->r.x + 4, cy - 2, w->r.w - 8, 4, 0xFFFFFFFF);
        ui_bevel(s, mkrect(w->r.x + 4, cy - 2, w->r.w - 8, 4), true);
        rect_t k = mkrect(px - 5, cy - 10, 11, 20);
        gfx_fill(s, k.x, k.y, k.w, k.h, ui_theme.button_bg);
        ui_bevel(s, k, false);
        return;
    }
    gfx_fill_rounded(s, w->r.x + 8, cy - 2, w->r.w - 16, 4, 2, ui_theme.scroll_track);
    gfx_fill_rounded(s, w->r.x + 8, cy - 2, px - w->r.x - 8, 4, 2, ui_theme.accent);
    gfx_fill_circle(s, px, cy, 8, ui_theme.accent);
    gfx_fill_circle(s, px, cy, w->win->hover == w ? 5 : 4, ui_theme.input_bg);
}

static void draw_dropdown(ui_widget_t *w, surface_t *s) {
    dropdown_t *d = w->data;
    bool hover = w->win->hover == w;
    ui_draw_button_frame(s, w->r, hover ? 1 : 0, false);
    const char *t = w->ival >= 0 && w->ival < d->count ? d->items[w->ival] : "";
    font_draw_fit(s, w->font, w->r.x + 10, w->r.y + (w->r.h - w->font->height) / 2, t, w->r.w - 36,
                  ui_theme.button_text);
    ui_draw_glyph(s, "chev-down", 16, w->r.x + w->r.w - 24, w->r.y + (w->r.h - 16) / 2, ui_theme.button_text);
    draw_focus(s, w);
}

void ui_widget_draw(ui_widget_t *w, surface_t *s) {
    switch (w->type) {
    case W_LABEL: {
        uint32_t c = w->color ? w->color : (w->enabled ? ui_theme.window_text : ui_theme.window_text_dim);
        const font_t *f = w->font;
        /* multi-line labels: split at '\n' and wrap words */
        int y = w->r.y;
        const char *p = w->text;
        bool single = !strchr(p, '\n') && font_text_width(f, p) <= w->r.w;
        if (single) {
            int ty = w->r.h < f->height * 2 ? w->r.y + (w->r.h - f->height) / 2 : w->r.y;
            int tw = font_text_width(f, p);
            int x = w->align == 1 ? w->r.x + (w->r.w - tw) / 2 : w->align == 2 ? w->r.x + w->r.w - tw : w->r.x;
            font_draw(s, f, x, ty, p, c);
            break;
        }
        while (*p && y < w->r.y + w->r.h) {
            int n = 0, last_space = -1;
            while (p[n] && p[n] != '\n') {
                if (p[n] == ' ') last_space = n;
                int nn = utf8_next(p, n);
                if (font_text_width_n(f, p, nn) > w->r.w && n > 0) {
                    if (last_space > 0) n = last_space;
                    break;
                }
                n = nn;
            }
            int tw = font_text_width_n(f, p, n);
            int x = w->align == 1 ? w->r.x + (w->r.w - tw) / 2 : w->align == 2 ? w->r.x + w->r.w - tw : w->r.x;
            font_draw_n(s, f, x, y, p, n, c);
            y += f->height + 2;
            p += n;
            if (*p == ' ' || *p == '\n') p++;
        }
        break;
    }
    case W_BUTTON: {
        button_t *b = w->data;
        int state = !w->enabled ? 3 : (b->pressed && w->win->hover == w) ? 2 : w->win->hover == w ? 1 : 0;
        bool primary = w->ival == BTN_PRIMARY;
        ui_draw_button_frame(s, w->r, state, primary);
        uint32_t tc = primary && !ui_is_retro() ? ui_theme.accent_text : ui_theme.button_text;
        if (!w->enabled) tc = ui_theme.window_text_dim;
        rect_t r = w->r;
        if (state == 2 && ui_is_retro()) { r.x++; r.y++; }
        if (w->glyph[0]) {
            int tw = font_text_width(w->font, w->text);
            int total = 16 + (tw ? 8 + tw : 0);
            int x = r.x + (r.w - total) / 2;
            ui_draw_glyph(s, w->glyph, 16, x, r.y + (r.h - 16) / 2, tc);
            if (tw) font_draw(s, w->font, x + 24, r.y + (r.h - w->font->height) / 2, w->text, tc);
        } else {
            ui_draw_text_center(s, w->font, r, w->text, tc);
        }
        draw_focus(s, w);
        break;
    }
    case W_TOOLBUTTON: {
        button_t *b = w->data;
        bool hover = w->win->hover == w;
        bool active = w->ival != 0;
        uint32_t col = w->enabled ? ui_theme.window_text : ui_theme.window_text_dim;
        if (ui_is_retro()) {
            if (active || (hover && w->enabled)) ui_bevel(s, w->r, active || b->pressed);
        } else if (active) {
            gfx_fill_rounded(s, w->r.x, w->r.y, w->r.w, w->r.h, 6, WITH_ALPHA(ui_theme.accent, 70));
            gfx_rounded_rect(s, w->r.x, w->r.y, w->r.w, w->r.h, 6, WITH_ALPHA(ui_theme.accent, 180));
        } else if (hover && w->enabled) {
            gfx_fill_rounded(s, w->r.x, w->r.y, w->r.w, w->r.h, 6, b->pressed ? ui_theme.button_pressed : ui_theme.button_hover);
        }
        int gs = w->r.w >= 32 ? 20 : 16;
        ui_draw_glyph(s, w->glyph, gs, w->r.x + (w->r.w - gs) / 2, w->r.y + (w->r.h - gs) / 2, col);
        break;
    }
    case W_TEXTBOX: draw_textbox(w, s); break;
    case W_CHECKBOX: draw_checkbox(w, s); break;
    case W_SLIDER: draw_slider(w, s); draw_focus(s, w); break;
    case W_LIST: draw_list(w, s); break;
    case W_DROPDOWN: draw_dropdown(w, s); break;
    case W_PROGRESS: {
        int range = MAX(1, w->imax);
        int fw = (w->r.w - 2) * MIN(w->ival, range) / range;
        if (ui_is_retro()) {
            gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, 0xFFFFFFFF);
            ui_bevel(s, w->r, true);
            for (int x = 0; x + 8 <= fw; x += 10) gfx_fill(s, w->r.x + 2 + x, w->r.y + 2, 8, w->r.h - 4, ui_theme.accent);
        } else {
            int rad = w->r.h / 2;
            gfx_fill_rounded(s, w->r.x, w->r.y, w->r.w, w->r.h, rad, ui_theme.scroll_track);
            if (fw > 0) gfx_fill_rounded(s, w->r.x, w->r.y, MAX(fw, w->r.h), w->r.h, rad, w->color ? w->color : ui_theme.accent);
        }
        if (w->text[0]) ui_draw_text_center(s, w->font, w->r, w->text, ui_theme.window_text);
        break;
    }
    case W_CANVAS:
        if (w->draw) w->draw(w, s);
        break;
    case W_MENUBAR:
        ui_menubar_draw(w, s);
        break;
    case W_PANEL:
        gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, w->color ? w->color : ui_theme.sidebar_bg);
        break;
    case W_SEPARATOR:
        gfx_hline(s, w->r.x, w->r.y + w->r.h / 2, w->r.w, ui_theme.input_border);
        break;
    case W_SWATCH: {
        bool hover = w->win->hover == w;
        if (ui_is_retro()) {
            gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, w->color);
            ui_bevel(s, w->r, true);
        } else {
            int rad = MIN(w->r.w, w->r.h) / 2;
            gfx_fill_rounded(s, w->r.x, w->r.y, w->r.w, w->r.h, MIN(rad, 6), w->color | 0xFF000000u);
            gfx_rounded_rect(s, w->r.x, w->r.y, w->r.w, w->r.h, MIN(rad, 6),
                             w->ival ? ui_theme.window_text : hover ? ui_theme.accent : ui_theme.input_border);
            if (w->ival) gfx_rounded_rect(s, w->r.x + 1, w->r.y + 1, w->r.w - 2, w->r.h - 2, MIN(rad, 6), ui_theme.window_bg);
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ events */
static void tb_changed(ui_widget_t *w) {
    ui_widget_invalidate(w);
    if (w->on_change) w->on_change(w);
}

static void tb_delete_sel(textbox_t *t) {
    int a = MIN(t->cursor, t->anchor), b = MAX(t->cursor, t->anchor);
    if (a == b) return;
    memmove(t->buf + a, t->buf + b, t->len - b + 1);
    t->len -= b - a;
    t->cursor = t->anchor = a;
}

static void tb_insert(textbox_t *t, const char *s, int n) {
    tb_delete_sel(t);
    if (t->len + n + 1 > t->cap) {
        t->cap = t->len + n + 64;
        t->buf = realloc(t->buf, t->cap);
    }
    memmove(t->buf + t->cursor + n, t->buf + t->cursor, t->len - t->cursor + 1);
    memcpy(t->buf + t->cursor, s, n);
    t->len += n;
    t->cursor += n;
    t->anchor = t->cursor;
}

static int tb_index_at(ui_widget_t *w, int x) {
    textbox_t *t = w->data;
    if (t->password) {
        int px = x - (w->r.x + 7) + t->scroll;
        int bullet = font_text_width(w->font, "\xE2\x80\xA2");
        int n = bullet ? (px + bullet / 2) / bullet : 0;
        int i = 0;
        while (n-- > 0 && i < t->len) i = utf8_next(t->buf, i);
        return i;
    }
    return font_index_at(w->font, t->buf, x - (w->r.x + 7) + t->scroll);
}

static bool textbox_event(ui_widget_t *w, gui_event_t *ev) {
    textbox_t *t = w->data;
    if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) {
        if (ev->clicks >= 2) { ui_textbox_select_all(w); return true; }
        t->cursor = t->anchor = tb_index_at(w, ev->x);
        t->dragging = true;
        ui_widget_invalidate(w);
        return true;
    }
    if (ev->type == EV_MOUSE_MOVE && t->dragging) {
        t->cursor = tb_index_at(w, ev->x);
        ui_widget_invalidate(w);
        return true;
    }
    if (ev->type == EV_MOUSE_UP) { t->dragging = false; return true; }
    if (ev->type != EV_KEY_DOWN) return false;
    bool shift = ev->mods & MOD_SHIFT, ctrl = ev->mods & MOD_CTRL;
    uint32_t ch = ev->ch;
    if (ctrl && (ch == 'a' || ch == 'A')) { ui_textbox_select_all(w); return true; }
    if (ctrl && (ch == 'c' || ch == 'C' || ch == 'x' || ch == 'X')) {
        int a = MIN(t->cursor, t->anchor), b = MAX(t->cursor, t->anchor);
        if (a != b && !t->password) gui_clipboard_set(t->buf + a, b - a);
        if ((ch == 'x' || ch == 'X') && a != b) { tb_delete_sel(t); tb_changed(w); }
        return true;
    }
    if (ctrl && (ch == 'v' || ch == 'V')) {
        long n = gui_clipboard_get(0, 0);
        if (n > 0) {
            char *buf = malloc(n + 1);
            gui_clipboard_get(buf, n);
            buf[n] = 0;
            for (long i = 0; i < n; i++) if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') buf[i] = ' ';
            tb_insert(t, buf, (int)n);
            free(buf);
            tb_changed(w);
        }
        return true;
    }
    switch (ev->key) {
    case KEY_LEFT:
        if (t->cursor > 0) t->cursor = utf8_prev(t->buf, t->cursor);
        if (!shift) t->anchor = t->cursor;
        ui_widget_invalidate(w);
        return true;
    case KEY_RIGHT:
        if (t->cursor < t->len) t->cursor = utf8_next(t->buf, t->cursor);
        if (!shift) t->anchor = t->cursor;
        ui_widget_invalidate(w);
        return true;
    case KEY_HOME:
        t->cursor = 0;
        if (!shift) t->anchor = 0;
        ui_widget_invalidate(w);
        return true;
    case KEY_END:
        t->cursor = t->len;
        if (!shift) t->anchor = t->len;
        ui_widget_invalidate(w);
        return true;
    case KEY_BACKSPACE:
        if (t->cursor != t->anchor) tb_delete_sel(t);
        else if (t->cursor > 0) {
            t->anchor = utf8_prev(t->buf, t->cursor);
            tb_delete_sel(t);
        }
        tb_changed(w);
        return true;
    case KEY_DELETE:
        if (t->cursor != t->anchor) tb_delete_sel(t);
        else if (t->cursor < t->len) {
            t->anchor = utf8_next(t->buf, t->cursor);
            tb_delete_sel(t);
        }
        tb_changed(w);
        return true;
    case KEY_ENTER: case KEY_KPENTER:
        if (w->on_activate) w->on_activate(w);
        return true;
    case KEY_ESC:
        return false;
    }
    if (ch >= ' ' && !ctrl && ch != 127) {
        char enc[4];
        int n = utf8_encode(ch, enc);
        tb_insert(t, enc, n);
        tb_changed(w);
        return true;
    }
    return false;
}

static void dropdown_popup(ui_widget_t *w);

static bool list_event(ui_widget_t *w, gui_event_t *ev) {
    list_t *l = w->data;
    int old = w->ival;
    switch (ev->type) {
    case EV_MOUSE_WHEEL:
        l->scroll += ev->wheel * (l->grid ? TILE_H / 2 : l->row_h * 3);
        list_clamp_scroll(w);
        ui_widget_invalidate(w);
        return true;
    case EV_MOUSE_MOVE: {
        int h = list_index_at(w, ev->x, ev->y);
        if (h != l->hover) { l->hover = h; ui_widget_invalidate(w); }
        return true;
    }
    case EV_MOUSE_LEAVE:
        if (l->hover != -1) { l->hover = -1; ui_widget_invalidate(w); }
        return true;
    case EV_MOUSE_DOWN: {
        int i = list_index_at(w, ev->x, ev->y);
        if (i >= 0 || ev->button == MOUSE_LEFT) w->ival = i;
        ui_widget_invalidate(w);
        if (w->ival != old && w->on_change) w->on_change(w);
        if (ev->button == MOUSE_LEFT && ev->clicks >= 2 && i >= 0 && w->on_activate) w->on_activate(w);
        return true;
    }
    case EV_KEY_DOWN: {
        int per_row = l->grid ? list_cols_per_row(w) : 1;
        int page = MAX(1, list_view_h(w) / (l->grid ? TILE_H : l->row_h)) * per_row;
        int i = w->ival;
        switch (ev->key) {
        case KEY_DOWN: i = i < 0 ? 0 : i + per_row; break;
        case KEY_UP: i = i < 0 ? 0 : i - per_row; break;
        case KEY_RIGHT: if (!l->grid) return false; i++; break;
        case KEY_LEFT: if (!l->grid) return false; i--; break;
        case KEY_PGDN: i += page; break;
        case KEY_PGUP: i -= page; break;
        case KEY_HOME: i = 0; break;
        case KEY_END: i = l->count - 1; break;
        case KEY_ENTER: case KEY_KPENTER:
            if (w->ival >= 0 && w->on_activate) w->on_activate(w);
            return true;
        default: return false;
        }
        if (l->count == 0) return true;
        i = MAX(0, MIN(i, l->count - 1));
        ui_list_select(w, i);
        if (w->ival != old && w->on_change) w->on_change(w);
        return true;
    }
    }
    return false;
}

bool ui_menubar_event(ui_widget_t *w, gui_event_t *ev);
void ui_menubar_draw(ui_widget_t *w, surface_t *s);

bool ui_widget_event(ui_widget_t *w, gui_event_t *ev) {
    switch (w->type) {
    case W_BUTTON: case W_TOOLBUTTON: case W_SWATCH: {
        button_t *b = w->data;
        if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) {
            if (b) b->pressed = true;
            ui_widget_invalidate(w);
            return true;
        }
        if (ev->type == EV_MOUSE_UP && ev->button == MOUSE_LEFT) {
            bool was = b ? b->pressed : true;
            if (b) b->pressed = false;
            ui_widget_invalidate(w);
            if (was && rect_contains(w->r, ev->x, ev->y) && w->on_click) w->on_click(w);
            return true;
        }
        if (ev->type == EV_KEY_DOWN && (ev->key == KEY_SPACE || ev->key == KEY_ENTER) && w->type == W_BUTTON) {
            if (w->on_click) w->on_click(w);
            return true;
        }
        return ev->type != EV_KEY_DOWN;
    }
    case W_TEXTBOX: return textbox_event(w, ev);
    case W_CHECKBOX:
        if ((ev->type == EV_MOUSE_UP && ev->button == MOUSE_LEFT && rect_contains(w->r, ev->x, ev->y)) ||
            (ev->type == EV_KEY_DOWN && ev->key == KEY_SPACE)) {
            w->ival = !w->ival;
            ui_widget_invalidate(w);
            if (w->on_change) w->on_change(w);
            return true;
        }
        return ev->type != EV_KEY_DOWN;
    case W_SLIDER: {
        int old = w->ival;
        if ((ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) || (ev->type == EV_MOUSE_MOVE && (ev->buttons & MOUSE_LEFT))) {
            int range = w->imax - w->imin;
            int v = w->imin + (ev->x - w->r.x - 8) * range / MAX(1, w->r.w - 16);
            w->ival = MAX(w->imin, MIN(w->imax, v));
        } else if (ev->type == EV_KEY_DOWN && (ev->key == KEY_LEFT || ev->key == KEY_RIGHT)) {
            int step = MAX(1, (w->imax - w->imin) / 20);
            w->ival = MAX(w->imin, MIN(w->imax, w->ival + (ev->key == KEY_RIGHT ? step : -step)));
        } else if (ev->type == EV_MOUSE_UP) {
            if (w->on_activate) w->on_activate(w);
            return true;
        } else {
            return ev->type != EV_KEY_DOWN;
        }
        ui_widget_invalidate(w);
        if (w->ival != old && w->on_change) w->on_change(w);
        return true;
    }
    case W_LIST: return list_event(w, ev);
    case W_DROPDOWN:
        if ((ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) ||
            (ev->type == EV_KEY_DOWN && (ev->key == KEY_SPACE || ev->key == KEY_ENTER))) {
            dropdown_popup(w);
            return true;
        }
        if (ev->type == EV_KEY_DOWN && (ev->key == KEY_DOWN || ev->key == KEY_UP)) {
            dropdown_t *d = w->data;
            int i = w->ival + (ev->key == KEY_DOWN ? 1 : -1);
            if (i >= 0 && i < d->count) {
                w->ival = i;
                ui_widget_invalidate(w);
                if (w->on_change) w->on_change(w);
            }
            return true;
        }
        return ev->type != EV_KEY_DOWN;
    case W_CANVAS:
        return w->event ? w->event(w, ev) : false;
    case W_MENUBAR:
        return ui_menubar_event(w, ev);
    }
    return false;
}

/* ------------------------------------------------------------------ dropdown popup */
static ui_widget_t *dd_owner;

static void dd_pick(ui_widget_t *lw) {
    ui_widget_t *w = dd_owner;
    ui_window_t *pw = lw->win;
    if (w && lw->ival >= 0) {
        w->ival = lw->ival;
        ui_widget_invalidate(w);
        if (w->on_change) w->on_change(w);
    }
    dd_owner = 0;
    ui_window_close(pw);
}

static void dd_key(ui_window_t *win, gui_event_t *ev) {
    if (ev->key == KEY_ESC) { dd_owner = 0; ui_window_close(win); }
}

static void dropdown_popup(ui_widget_t *w) {
    dropdown_t *d = w->data;
    if (!d->count) return;
    int rect[8];
    if (gui_win_get_rect(w->win->id, rect) < 0) return;
    int rows = MIN(d->count, 10);
    gui_wincreate_t req;
    memset(&req, 0, sizeof(req));
    req.x = rect[4] + w->r.x;
    req.y = rect[5] + w->r.y + w->r.h + 2;
    req.w = MAX(w->r.w, 120);
    req.h = rows * 28 + 4;
    req.flags = WF_POPUP | WF_NO_DECOR | WF_NO_TASKBAR;
    req.parent = w->win->id;
    strlcpy(req.title, "dropdown", sizeof(req.title));
    ui_window_t *pw = ui_window_ex(&req);
    if (!pw) return;
    pw->bg = ui_theme.menu_bg;
    pw->owner = w->win;
    ui_widget_t *l = ui_list(pw, 0, 0, req.w, req.h);
    ui_list_set_row_height(l, 28);
    for (int i = 0; i < d->count; i++) ui_list_add(l, d->items[i], 0, 0);
    ui_list_select(l, w->ival);
    l->on_change = 0;
    l->on_activate = dd_pick;
    /* a single click picks */
    l->on_change = dd_pick;
    pw->on_key = dd_key;
    ui_focus(pw, l);
    dd_owner = w;
}
