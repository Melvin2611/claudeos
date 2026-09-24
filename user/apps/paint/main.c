/* Paint */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <claudeos.h>

#define TOOLBAR_H 52
#define STATUS_H 24
#define UNDO_MAX 12
#define ABSI(x) ((x) < 0 ? -(x) : (x))

enum { T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_PICKER, T_COUNT };
static const char *tool_glyphs[T_COUNT] = { "pencil", "brush", "eraser", "line", "rect", "ellipse", "fill", "picker" };
static const char *tool_names[T_COUNT] = { "Pencil", "Brush", "Eraser", "Line", "Rectangle", "Ellipse", "Fill", "Color picker" };

static ui_window_t *win;
static ui_widget_t *canvas, *tool_btns[T_COUNT], *size_slider, *fill_cb, *status, *color_sw, *color2_sw;
static surface_t pic;
static uint32_t *pix;
static int pw = 800, ph = 520;
static int tool = T_BRUSH;
static uint32_t fg = 0xFF1C1D21, bg = 0xFFFFFFFF;
static int brush = 4;
static bool drawing;
static int last_x, last_y, start_x, start_y, cur_x, cur_y;
static uint32_t draw_color;
static uint32_t *undo[UNDO_MAX];
static int nundo;
static char path[256];
static bool modified;
static int off_x, off_y;   /* image offset inside the canvas (scrolling) */

static const uint32_t palette[] = {
    0x1C1D21, 0x7F7F7F, 0xFFFFFF, 0xC3C3C3, 0xE5484D, 0xB91C1C, 0xF5A623, 0xFDE047, 0x30A46C, 0x86EFAC,
    0x0A84FF, 0x7DD3FC, 0x8E4EC6, 0xF0ABFC, 0xD97757, 0x8B5A2B,
};

static void update_title(void) {
    char t[300];
    snprintf(t, sizeof(t), "%s%s - Paint", modified ? "\xE2\x80\xA2 " : "", path[0] ? ui_basename(path) : "Untitled");
    ui_set_title(win, t);
}

static void push_undo(void) {
    if (nundo == UNDO_MAX) { free(undo[0]); memmove(undo, undo + 1, sizeof(uint32_t *) * (UNDO_MAX - 1)); nundo--; }
    uint32_t *copy = malloc((size_t)pw * ph * 4);
    memcpy(copy, pix, (size_t)pw * ph * 4);
    undo[nundo++] = copy;
}

static void do_undo(void) {
    if (!nundo) return;
    uint32_t *u = undo[--nundo];
    memcpy(pix, u, (size_t)pw * ph * 4);
    free(u);
    ui_widget_invalidate(canvas);
}

static void new_image(int w, int h) {
    free(pix);
    pw = w;
    ph = h;
    pix = malloc((size_t)w * h * 4);
    gfx_init(&pic, pix, w, h, w);
    gfx_fill(&pic, 0, 0, w, h, 0xFFFFFFFF);
    for (int i = 0; i < nundo; i++) free(undo[i]);
    nundo = 0;
    off_x = off_y = 0;
}

static int img_x(int x) { return x - canvas->r.x - 8 + off_x; }
static int img_y(int y) { return y - canvas->r.y - 8 + off_y; }

static void stamp_line(int x0, int y0, int x1, int y1, uint32_t c, int size) {
    if (tool == T_PENCIL) gfx_line(&pic, x0, y0, x1, y1, c);
    else gfx_thick_line(&pic, x0, y0, x1, y1, size, c);
}

static void flood_fill(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= pw || y >= ph) return;
    uint32_t target = pix[y * pw + x];
    if (target == (c | 0xFF000000u)) return;
    c |= 0xFF000000u;
    int cap = 4096, n = 0;
    int *stack = malloc(sizeof(int) * 2 * cap);
    stack[n++] = x;
    stack[n++] = y;
    while (n) {
        int py = stack[--n], px = stack[--n];
        int l = px;
        while (l > 0 && pix[py * pw + l - 1] == target) l--;
        int r = px;
        while (r < pw - 1 && pix[py * pw + r + 1] == target) r++;
        for (int i = l; i <= r; i++) pix[py * pw + i] = c;
        for (int dy = -1; dy <= 1; dy += 2) {
            int ny = py + dy;
            if (ny < 0 || ny >= ph) continue;
            bool in_run = false;
            for (int i = l; i <= r; i++) {
                bool match = pix[ny * pw + i] == target;
                if (match && !in_run) {
                    if (n + 2 > cap * 2) { cap *= 2; stack = realloc(stack, sizeof(int) * 2 * cap); }
                    stack[n++] = i;
                    stack[n++] = ny;
                }
                in_run = match;
            }
        }
    }
    free(stack);
}

static void shape(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c, int dx, int dy) {
    int ax = MIN(x0, x1) + dx, ay = MIN(y0, y1) + dy, w = ABSI(x1 - x0) + 1, h = ABSI(y1 - y0) + 1;
    switch (tool) {
    case T_LINE: gfx_thick_line(s, x0 + dx, y0 + dy, x1 + dx, y1 + dy, brush, c); break;
    case T_RECT:
        if (fill_cb->ival) gfx_fill(s, ax, ay, w, h, c);
        else for (int i = 0; i < brush && i * 2 < MIN(w, h); i++) gfx_rect(s, ax + i, ay + i, w - 2 * i, h - 2 * i, c);
        break;
    case T_ELLIPSE:
        if (fill_cb->ival) gfx_fill_ellipse(s, ax, ay, w, h, c);
        else for (int i = 0; i < brush && i * 2 < MIN(w, h); i++) gfx_ellipse(s, ax + i, ay + i, w - 2 * i, h - 2 * i, c);
        break;
    }
}

static void draw(ui_widget_t *w, surface_t *s) {
    gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, ui_theme.dark ? 0xFF2A2B30 : 0xFFD8DBE0);
    surface_t sub = *s;
    rect_t cl;
    if (!rect_intersect(s->clip, w->r, &cl)) return;
    gfx_set_clip(&sub, cl);
    int x = w->r.x + 8 - off_x, y = w->r.y + 8 - off_y;
    gfx_shadow(&sub, mkrect(x, y, pw, ph), 0, 8, 90, 2);
    gfx_blit(&sub, x, y, &pic, 0, 0, pw, ph);
    if (drawing && (tool == T_LINE || tool == T_RECT || tool == T_ELLIPSE)) {
        rect_t ic;
        rect_intersect(cl, mkrect(x, y, pw, ph), &ic);
        gfx_set_clip(&sub, ic);
        shape(&sub, start_x, start_y, cur_x, cur_y, draw_color, x, y);
    }
    /* brush outline */
    if (!drawing && (tool == T_BRUSH || tool == T_ERASER) && w->win->hover == w && cur_x >= 0) {
        gfx_set_clip(&sub, cl);
        gfx_circle(&sub, x + cur_x, y + cur_y, MAX(1, brush / 2), 0x80000000);
    }
}

static void update_status(void) {
    char t[160];
    snprintf(t, sizeof(t), "%s  \xE2\x80\xA2  size %d  \xE2\x80\xA2  %d x %d px  \xE2\x80\xA2  cursor %d, %d", tool_names[tool], brush, pw, ph,
             cur_x, cur_y);
    ui_set_text(status, t);
}

static bool event(ui_widget_t *w, gui_event_t *ev) {
    int x = img_x(ev->x), y = img_y(ev->y);
    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        if (ev->button != MOUSE_LEFT && ev->button != MOUSE_RIGHT) return true;
        draw_color = ev->button == MOUSE_LEFT ? fg : bg;
        if (tool == T_ERASER) draw_color = bg;
        if (tool == T_PICKER) {
            if (x >= 0 && y >= 0 && x < pw && y < ph) {
                uint32_t c = pix[y * pw + x];
                if (ev->button == MOUSE_LEFT) { fg = c; color_sw->color = c; ui_widget_invalidate(color_sw); }
                else { bg = c; color2_sw->color = c; ui_widget_invalidate(color2_sw); }
            }
            return true;
        }
        push_undo();
        modified = true;
        update_title();
        if (tool == T_FILL) {
            flood_fill(x, y, draw_color);
            ui_widget_invalidate(w);
            return true;
        }
        drawing = true;
        start_x = last_x = cur_x = x;
        start_y = last_y = cur_y = y;
        if (tool == T_PENCIL || tool == T_BRUSH || tool == T_ERASER) {
            stamp_line(x, y, x, y, draw_color, tool == T_ERASER ? brush * 2 : brush);
        }
        ui_widget_invalidate(w);
        return true;
    }
    case EV_MOUSE_MOVE:
        cur_x = x;
        cur_y = y;
        if (drawing && (tool == T_PENCIL || tool == T_BRUSH || tool == T_ERASER)) {
            stamp_line(last_x, last_y, x, y, draw_color, tool == T_ERASER ? brush * 2 : brush);
            last_x = x;
            last_y = y;
        }
        ui_widget_invalidate(w);
        update_status();
        return true;
    case EV_MOUSE_UP:
        if (drawing) {
            if (tool == T_LINE || tool == T_RECT || tool == T_ELLIPSE) shape(&pic, start_x, start_y, x, y, draw_color, 0, 0);
            drawing = false;
            ui_widget_invalidate(w);
        }
        return true;
    case EV_MOUSE_WHEEL:
        if (ev->mods & MOD_SHIFT) off_x = MAX(0, MIN(MAX(0, pw - w->r.w + 16), off_x + ev->wheel * 40));
        else off_y = MAX(0, MIN(MAX(0, ph - w->r.h + 16), off_y + ev->wheel * 40));
        ui_widget_invalidate(w);
        return true;
    case EV_MOUSE_LEAVE:
        cur_x = -1;
        ui_widget_invalidate(w);
        return true;
    }
    return false;
}

static void select_tool(int t) {
    tool = t;
    for (int i = 0; i < T_COUNT; i++) { tool_btns[i]->ival = i == t; ui_widget_invalidate(tool_btns[i]); }
    ui_set_cursor(win, t == T_PICKER || t == T_FILL ? CUR_HAND : CUR_CROSS);
    canvas->cursor = t == T_PICKER || t == T_FILL ? CUR_HAND : CUR_CROSS;
    update_status();
}

static void on_tool(ui_widget_t *w) { select_tool(w->id); }
static void on_size(ui_widget_t *w) { brush = w->ival; update_status(); }

static void on_palette(ui_widget_t *w) {
    fg = w->color;
    color_sw->color = fg;
    ui_widget_invalidate(color_sw);
}

static void on_custom_color(ui_widget_t *w) {
    uint32_t *target = w == color2_sw ? &bg : &fg;
    uint32_t c = *target;
    if (ui_color_dialog(win, &c)) {
        *target = c;
        w->color = c;
        ui_widget_invalidate(w);
    }
}

/* ------------------------------------------------------------------ files */
static bool save_to(const char *p) {
    size_t sz = bmp_encoded_size(pw, ph);
    void *buf = malloc(sz);
    bmp_encode(pix, pw, ph, buf);
    bool ok = ui_write_file(p, buf, sz);
    free(buf);
    if (!ok) { ui_msgbox(win, "Paint", "Could not save the picture.", "OK"); return false; }
    strlcpy(path, p, sizeof(path));
    modified = false;
    update_title();
    return true;
}

static bool save_as(void) {
    char *p = ui_file_dialog(win, true, "Save Picture", "/home/Pictures", path[0] ? ui_basename(path) : "Drawing.bmp", "bmp");
    if (!p) return false;
    if (!strrchr(ui_basename(p), '.')) {
        char q[300];
        snprintf(q, sizeof(q), "%s.bmp", p);
        free(p);
        p = strdup(q);
    }
    bool r = save_to(p);
    free(p);
    return r;
}

static bool save(void) { return path[0] ? save_to(path) : save_as(); }

static bool confirm(void) {
    if (!modified) return true;
    int r = ui_msgbox(win, "Paint", "Do you want to save your drawing?", "Save|Don't Save|Cancel");
    if (r == 0) return save();
    return r == 1;
}

static bool load(const char *p) {
    size_t sz;
    char *data = ui_read_file(p, &sz);
    int w, h;
    if (!data || !bmp_info(data, sz, &w, &h)) { free(data); return false; }
    new_image(w, h);
    bmp_decode(data, sz, pix);
    for (int i = 0; i < w * h; i++) pix[i] |= 0xFF000000u;
    free(data);
    strlcpy(path, p, sizeof(path));
    modified = false;
    update_title();
    ui_widget_invalidate(canvas);
    return true;
}

enum { M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_EXIT, M_UNDO, M_CLEAR, M_ABOUT };

static void menu_cb(ui_window_t *w, int id) {
    (void)w;
    switch (id) {
    case M_NEW:
        if (confirm()) { new_image(MAX(200, canvas->r.w - 16), MAX(200, canvas->r.h - 16)); path[0] = 0; modified = false; update_title(); ui_widget_invalidate(canvas); }
        break;
    case M_OPEN:
        if (confirm()) {
            char *p = ui_file_dialog(win, false, "Open Picture", "/home/Pictures", 0, "bmp");
            if (p) { if (!load(p)) ui_msgbox(win, "Paint", "Could not open the picture.", "OK"); free(p); }
        }
        break;
    case M_SAVE: save(); break;
    case M_SAVEAS: save_as(); break;
    case M_EXIT: if (confirm()) { ui_window_close(win); ui_quit(); } break;
    case M_UNDO: do_undo(); break;
    case M_CLEAR: push_undo(); gfx_fill(&pic, 0, 0, pw, ph, bg); modified = true; update_title(); ui_widget_invalidate(canvas); break;
    case M_ABOUT: ui_msgbox(win, "About Paint", "Paint for ClaudeOS\n\nLeft click paints with the primary color, right click with the secondary color.", "OK"); break;
    }
}

static void on_key(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->mods & MOD_CTRL) {
        switch (ev->ch | 0x20) {
        case 'z': do_undo(); return;
        case 's': save(); return;
        case 'o': menu_cb(win, M_OPEN); return;
        case 'n': menu_cb(win, M_NEW); return;
        }
    }
    const char *keys = "pbeloifk";
    for (int i = 0; i < T_COUNT; i++) if ((uint32_t)keys[i] == ev->ch) select_tool(i);
    if (ev->ch == '+') { size_slider->ival = MIN(40, brush + 1); on_size(size_slider); ui_widget_invalidate(size_slider); }
    if (ev->ch == '-') { size_slider->ival = MAX(1, brush - 1); on_size(size_slider); ui_widget_invalidate(size_slider); }
}

static bool on_close(ui_window_t *w) {
    (void)w;
    if (!confirm()) return false;
    ui_quit();
    return true;
}

static void paint_bg(ui_window_t *w, surface_t *s) {
    gfx_fill(s, 0, 28, w->w, TOOLBAR_H, ui_is_retro() ? ui_theme.window_bg : ui_theme.title_bg);
    gfx_hline(s, 0, 28 + TOOLBAR_H - 1, w->w, ui_theme.input_border);
}

int main(int argc, char **argv) {
    int W = 900, H = 640;
    win = ui_window("Paint", W, H, WF_RESIZABLE, "paint");
    if (!win) return 1;
    win->on_key = on_key;
    win->on_close = on_close;
    win->on_paint = paint_bg;
    ui_widget_t *bar = ui_menubar(win, menu_cb);
    ui_menu_t *mf = ui_menu_new();
    ui_menu_item(mf, "New", "Ctrl+N", M_NEW);
    ui_menu_item(mf, "Open...", "Ctrl+O", M_OPEN);
    ui_menu_item(mf, "Save", "Ctrl+S", M_SAVE);
    ui_menu_item(mf, "Save As...", 0, M_SAVEAS);
    ui_menu_separator(mf);
    ui_menu_item(mf, "Exit", 0, M_EXIT);
    ui_menubar_add(bar, "File", mf);
    ui_menu_t *me = ui_menu_new();
    ui_menu_item(me, "Undo", "Ctrl+Z", M_UNDO);
    ui_menu_item(me, "Clear Image", 0, M_CLEAR);
    ui_menubar_add(bar, "Edit", me);
    ui_menu_t *mh = ui_menu_new();
    ui_menu_item(mh, "About Paint", 0, M_ABOUT);
    ui_menubar_add(bar, "Help", mh);

    int y = 28 + 10;
    for (int i = 0; i < T_COUNT; i++) {
        tool_btns[i] = ui_toolbutton(win, 8 + i * 36, y, 32, tool_glyphs[i], tool_names[i], on_tool);
        tool_btns[i]->id = i;
    }
    int x = 8 + T_COUNT * 36 + 12;
    ui_label(win, x, y, 34, 32, "Size");
    size_slider = ui_slider(win, x + 34, y, 110, 32, 1, 40, brush);
    size_slider->on_change = on_size;
    fill_cb = ui_checkbox(win, x + 150, y, 70, 32, "Fill", false);
    x += 230;
    color_sw = ui_swatch(win, x, y - 2, 28, 28, fg, on_custom_color);
    color2_sw = ui_swatch(win, x + 16, y + 12, 22, 22, bg, on_custom_color);
    x += 50;
    for (int i = 0; i < 16; i++) {
        ui_widget_t *sw = ui_swatch(win, x + (i / 2) * 22, y + (i % 2) * 17 - 1, 19, 15, 0xFF000000u | palette[i], on_palette);
        (void)sw;
    }
    canvas = ui_canvas(win, 0, 28 + TOOLBAR_H, W, H - 28 - TOOLBAR_H - STATUS_H, draw, event);
    ui_set_anchor(canvas, A_ALL);
    status = ui_label(win, 10, H - STATUS_H, W - 20, STATUS_H, "");
    ui_set_anchor(status, A_LEFT | A_RIGHT | A_BOTTOM);
    new_image(W - 16, H - 28 - TOOLBAR_H - STATUS_H - 16);
    cur_x = -1;
    if (argc > 1 && argv[1][0]) load(argv[1]);
    select_tool(T_BRUSH);
    update_title();
    ui_focus(win, canvas);
    ui_run();
    return 0;
}
