/* Image Viewer (BMP and ClaudeOS icons) */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <claudeos.h>

#define TB_H 48

static ui_window_t *win;
static ui_widget_t *canvas, *info;
static uint32_t *img;
static int iw, ih;
static char path[256];
static int zoom = 0;           /* 0 = fit, otherwise percent */
static int pan_x, pan_y, drag_x, drag_y;
static bool dragging;

static bool load(const char *p) {
    size_t sz;
    char *data = ui_read_file(p, &sz);
    if (!data) return false;
    int w, h;
    const uint32_t *px;
    uint32_t *out = 0;
    if (bmp_info(data, sz, &w, &h)) {
        out = malloc((size_t)w * h * 4);
        bmp_decode(data, sz, out);
    } else if (icn_parse(data, sz, &w, &h, &px)) {
        out = malloc((size_t)w * h * 4);
        memcpy(out, px, (size_t)w * h * 4);
    }
    free(data);
    if (!out) return false;
    free(img);
    img = out;
    iw = w;
    ih = h;
    strlcpy(path, p, sizeof(path));
    zoom = 0;
    pan_x = pan_y = 0;
    char t[300];
    snprintf(t, sizeof(t), "%s - Image Viewer", ui_basename(p));
    ui_set_title(win, t);
    char sz_s[16];
    format_size(sz, sz_s, sizeof(sz_s));
    snprintf(t, sizeof(t), "%d x %d pixels  \xE2\x80\xA2  %s", w, h, sz_s);
    ui_set_text(info, t);
    ui_widget_invalidate(canvas);
    return true;
}

static int cur_scale(void) {   /* percent */
    if (zoom) return zoom;
    if (!iw) return 100;
    int sw = (canvas->r.w - 20) * 100 / iw, sh = (canvas->r.h - 20) * 100 / ih;
    int s = MIN(sw, sh);
    return MIN(s, 100);
}

static void draw(ui_widget_t *w, surface_t *s) {
    gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, ui_theme.dark ? 0xFF16171A : 0xFFE4E6EA);
    if (!img) {
        ui_draw_text_center(s, &ui_font_large, w->r, "Open an image (Ctrl+O)", ui_theme.window_text_dim);
        return;
    }
    int sc = cur_scale();
    int dw = MAX(1, iw * sc / 100), dh = MAX(1, ih * sc / 100);
    int x = w->r.x + (w->r.w - dw) / 2 + pan_x, y = w->r.y + (w->r.h - dh) / 2 + pan_y;
    surface_t src, sub = *s;
    gfx_init(&src, img, iw, ih, iw);
    rect_t cl;
    if (!rect_intersect(s->clip, w->r, &cl)) return;
    gfx_set_clip(&sub, cl);
    /* checkerboard behind transparent images */
    for (int yy = MAX(y, cl.y); yy < MIN(y + dh, cl.y + cl.h); yy += 8)
        for (int xx = MAX(x, cl.x); xx < MIN(x + dw, cl.x + cl.w); xx += 8)
            gfx_fill(&sub, xx, yy, 8, 8, (((xx - x) / 8 + (yy - y) / 8) & 1) ? 0xFFCCCCCC : 0xFFFFFFFF);
    gfx_blit_scaled(&sub, mkrect(x, y, dw, dh), &src, mkrect(0, 0, iw, ih), sc < 100, true);
}

static void zoom_by(int dir) {
    int sc = cur_scale();
    static const int steps[] = { 10, 25, 33, 50, 67, 75, 100, 150, 200, 300, 400, 800 };
    int n = sizeof(steps) / sizeof(steps[0]);
    int i = 0;
    if (dir > 0) { while (i < n - 1 && steps[i] <= sc) i++; }
    else { i = n - 1; while (i > 0 && steps[i] >= sc) i--; }
    zoom = steps[i];
    ui_widget_invalidate(canvas);
}

static bool event(ui_widget_t *w, gui_event_t *ev) {
    if (ev->type == EV_MOUSE_WHEEL) { zoom_by(-ev->wheel); return true; }
    if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT) {
        if (ev->clicks == 2) { zoom = zoom ? 0 : 100; pan_x = pan_y = 0; ui_widget_invalidate(w); return true; }
        dragging = true;
        drag_x = ev->x - pan_x;
        drag_y = ev->y - pan_y;
        ui_set_cursor(win, CUR_MOVE);
        return true;
    }
    if (ev->type == EV_MOUSE_MOVE && dragging) {
        pan_x = ev->x - drag_x;
        pan_y = ev->y - drag_y;
        ui_widget_invalidate(w);
        return true;
    }
    if (ev->type == EV_MOUSE_UP) { dragging = false; ui_set_cursor(win, CUR_ARROW); return true; }
    return false;
}

/* next/previous image in the same folder */
static void step(int dir) {
    if (!path[0]) return;
    char dir_s[256];
    strlcpy(dir_s, path, sizeof(dir_s));
    char *sl = strrchr(dir_s, '/');
    if (!sl) return;
    if (sl == dir_s) sl[1] = 0; else *sl = 0;
    DIR *d = opendir(dir_s);
    if (!d) return;
    char names[256][128];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && (!strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".icn"))) strlcpy(names[n++], e->d_name, 128);
    }
    closedir(d);
    if (!n) return;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcasecmp(names[i], names[j]) > 0) { char t[128]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t); }
    int cur = 0;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], ui_basename(path))) cur = i;
    int next = (cur + dir + n) % n;
    char p[400];
    snprintf(p, sizeof(p), "%s/%s", strcmp(dir_s, "/") ? dir_s : "", names[next]);
    load(p);
}

static void do_open(ui_widget_t *w) {
    (void)w;
    char *p = ui_file_dialog(win, false, "Open Image", "/home/Pictures", 0, "bmp;icn");
    if (p) {
        if (!load(p)) ui_msgbox(win, "Image Viewer", "The file is not a supported image (BMP).", "OK");
        free(p);
    }
}
static void do_prev(ui_widget_t *w) { (void)w; step(-1); }
static void do_next(ui_widget_t *w) { (void)w; step(1); }
static void do_zin(ui_widget_t *w) { (void)w; zoom_by(1); }
static void do_zout(ui_widget_t *w) { (void)w; zoom_by(-1); }
static void do_fit(ui_widget_t *w) { (void)w; zoom = 0; pan_x = pan_y = 0; ui_widget_invalidate(canvas); }
static void do_wallpaper(ui_widget_t *w) {
    (void)w;
    if (!path[0]) return;
    gui_config_t c;
    gui_config_get(&c);
    strlcpy(c.wp_image, path, sizeof(c.wp_image));
    c.wallpaper = WP_IMAGE;
    c.wp_mode = WPM_FILL;
    gui_config_set(&c, 0);
    gui_notify("Wallpaper", "The picture is now your desktop background.", "viewer");
}

static void on_key(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->key == KEY_RIGHT || ev->key == KEY_SPACE) step(1);
    else if (ev->key == KEY_LEFT || ev->key == KEY_BACKSPACE) step(-1);
    else if (ev->ch == '+') zoom_by(1);
    else if (ev->ch == '-') zoom_by(-1);
    else if (ev->ch == '0' || ev->ch == 'f') do_fit(0);
    else if ((ev->mods & MOD_CTRL) && (ev->ch == 'o' || ev->ch == 'O')) do_open(0);
}

int main(int argc, char **argv) {
    int W = 820, H = 600;
    win = ui_window("Image Viewer", W, H, WF_RESIZABLE, "viewer");
    if (!win) return 1;
    win->on_key = on_key;
    ui_toolbutton(win, 8, 8, 32, "open", "Open", do_open);
    ui_toolbutton(win, 48, 8, 32, "back", "Previous", do_prev);
    ui_toolbutton(win, 82, 8, 32, "forward", "Next", do_next);
    ui_toolbutton(win, 124, 8, 32, "zoom-out", "Zoom out", do_zout);
    ui_toolbutton(win, 158, 8, 32, "zoom-in", "Zoom in", do_zin);
    ui_toolbutton(win, 192, 8, 32, "grid", "Fit", do_fit);
    ui_widget_t *wp = ui_button(win, 236, 8, 150, 32, "Set as wallpaper", do_wallpaper);
    wp->focusable = false;
    info = ui_label(win, 400, 8, W - 410, 32, "");
    ui_set_anchor(info, A_LEFT | A_RIGHT | A_TOP);
    info->align = 2;
    canvas = ui_canvas(win, 0, TB_H, W, H - TB_H, draw, event);
    ui_set_anchor(canvas, A_ALL);
    ui_focus(win, canvas);
    if (argc > 1 && argv[1][0] && !load(argv[1])) ui_msgbox(win, "Image Viewer", "Could not open the image.", "OK");
    ui_run();
    return 0;
}
