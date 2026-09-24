/* Settings - personalize ClaudeOS */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/syscall.h>
#include <claudeos.h>

#define SIDE_W 210
#define PAGE_X (SIDE_W + 24)

enum { P_APPEARANCE, P_BACKGROUND, P_TASKBAR, P_DISPLAY, P_INPUT, P_SOUND, P_NETWORK, P_DATETIME, P_ABOUT, P_COUNT };
static const char *page_names[P_COUNT] = { "Appearance", "Background", "Taskbar", "Display", "Mouse & Keyboard",
                                           "Sound", "Network", "Date & Time", "About" };
static const char *page_icons[P_COUNT] = { "g:palette", "g:image", "g:taskbar", "g:computer", "g:keyboard",
                                           "g:volume", "g:net", "g:clock", "g:info" };

static ui_window_t *win;
static ui_widget_t *sidebar, *title_label;
static ui_widget_t *pages[P_COUNT][40];
static int npw[P_COUNT];
static int cur_page;
static gui_config_t cfg;
static int building;   /* page being built */

/* widgets that need updating */
static ui_widget_t *theme_canvas, *accent_swatches[10], *radius_slider, *radius_label, *shadows_cb, *transp_cb;
static ui_widget_t *wp_canvas, *wp_c1, *wp_c2, *wp_img_label, *wp_mode_dd, *wp_img_btn;
static ui_widget_t *tb_pos_dd, *tb_size_dd, *tb_labels_cb, *clk24_cb, *clksec_cb, *clkdate_cb, *icons_cb;
static ui_widget_t *res_dd, *res_label, *res_apply;
static ui_widget_t *speed_slider, *dbl_slider, *kbd_dd;
static ui_widget_t *vol_slider, *vol_label, *sounds_cb;
static ui_widget_t *net_label;
static ui_widget_t *dt_label, *dt_date, *dt_time;

static gui_theme_t presets[16];
static int npresets;

static const uint32_t accents[] = { 0, 0xD97757, 0x0A84FF, 0x30A46C, 0x8E4EC6, 0xE5484D, 0xF5A623, 0x12A594, 0xD6409F };

static const struct { int w, h; } resolutions[] = {
    { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 }, { 1366, 768 },
    { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
};
#define NRES (int)(sizeof(resolutions) / sizeof(resolutions[0]))

static const struct { const char *name; uint32_t c1, c2; } wp_presets[] = {
    { "Sunset", 0x2A1B3D, 0xD97757 }, { "Ocean", 0x0B2545, 0x13C4A3 }, { "Forest", 0x10261C, 0x6BBF59 },
    { "Night", 0x0B1020, 0x5B6BFF }, { "Rose", 0x3A1030, 0xF28AB2 }, { "Sand", 0x5C4A36, 0xF3D9A4 },
    { "Slate", 0x1E2530, 0x8A9BB0 },
};

static const char *wp_names[WP_COUNT] = { "Gradient", "Solid color", "Waves", "Aurora", "Mountains", "Bubbles", "Image" };

/* ------------------------------------------------------------------ helpers */
static ui_widget_t *add(ui_widget_t *w) {
    if (npw[building] < 40) pages[building][npw[building]++] = w;
    return w;
}

static void apply(void) { gui_config_set(&cfg, 0); }

static ui_widget_t *heading(int y, const char *text) {
    ui_widget_t *l = add(ui_label(win, PAGE_X, y, 400, 22, text));
    l->font = &ui_font_bold;
    return l;
}

static ui_widget_t *note(int y, const char *text) {
    ui_widget_t *l = add(ui_label(win, PAGE_X, y, win->w - PAGE_X - 24, 20, text));
    l->color = 0;
    l->font = &ui_font;
    return l;
}

static void show_page(int p) {
    cur_page = p;
    for (int i = 0; i < P_COUNT; i++)
        for (int k = 0; k < npw[i]; k++) ui_show(pages[i][k], i == p);
    ui_set_text(title_label, page_names[p]);
    ui_invalidate(win);
}

static void on_sidebar(ui_widget_t *w) { if (w->ival >= 0) show_page(w->ival); }

/* ------------------------------------------------------------------ appearance */
#define CARD_W 150
#define CARD_H 104

static int theme_index(void) {
    for (int i = 0; i < npresets; i++) if (!strcasecmp(presets[i].name, cfg.theme)) return i;
    return 0;
}

static void draw_theme_cards(ui_widget_t *w, surface_t *s) {
    int cols = MAX(1, w->r.w / (CARD_W + 12));
    int sel = theme_index();
    for (int i = 0; i < npresets; i++) {
        gui_theme_t *t = &presets[i];
        int x = w->r.x + (i % cols) * (CARD_W + 12), y = w->r.y + (i / cols) * (CARD_H + 30);
        gfx_fill_rounded(s, x, y, CARD_W, CARD_H, 8, t->desktop_bg);
        /* mini window */
        int wx = x + 14, wy = y + 14, ww = CARD_W - 28, wh = CARD_H - 34;
        gfx_fill_rounded(s, wx, wy, ww, wh, t->radius ? 5 : 0, t->window_bg);
        gfx_fill_rounded(s, wx, wy, ww, 14, t->radius ? 5 : 0, t->title_bg);
        gfx_fill(s, wx, wy + 9, ww, 5, t->title_bg);
        gfx_fill(s, wx + 8, wy + 22, ww / 2, 5, t->window_text);
        gfx_fill(s, wx + 8, wy + 32, ww / 3, 5, t->window_text_dim);
        gfx_fill_rounded(s, wx + ww - 40, wy + wh - 16, 32, 10, 3, t->accent);
        /* taskbar strip */
        gfx_fill(s, x, y + CARD_H - 12, CARD_W, 12, t->panel_bg);
        if (i == sel) {
            gfx_rounded_rect(s, x - 2, y - 2, CARD_W + 4, CARD_H + 4, 10, ui_theme.accent);
            gfx_rounded_rect(s, x - 3, y - 3, CARD_W + 6, CARD_H + 6, 11, ui_theme.accent);
        } else {
            gfx_rounded_rect(s, x, y, CARD_W, CARD_H, 8, ui_theme.input_border);
        }
        int tw = font_text_width(&ui_font, t->name);
        font_draw(s, i == sel ? &ui_font_bold : &ui_font, x + (CARD_W - tw) / 2, y + CARD_H + 6, t->name, ui_theme.window_text);
    }
}

static bool theme_cards_event(ui_widget_t *w, gui_event_t *ev) {
    if (ev->type != EV_MOUSE_DOWN || ev->button != MOUSE_LEFT) return false;
    int cols = MAX(1, w->r.w / (CARD_W + 12));
    int cx = (ev->x - w->r.x) / (CARD_W + 12), cy = (ev->y - w->r.y) / (CARD_H + 30);
    int i = cy * cols + cx;
    if (cx < cols && i >= 0 && i < npresets) {
        strlcpy(cfg.theme, presets[i].name, sizeof(cfg.theme));
        apply();
        ui_widget_invalidate(w);
    }
    return true;
}

static void update_accent_swatches(void) {
    for (int i = 0; i < (int)(sizeof(accents) / sizeof(accents[0])); i++) {
        if (!accent_swatches[i]) continue;
        accent_swatches[i]->ival = (cfg.accent & 0xFFFFFF) == accents[i] || (!cfg.accent && !accents[i]);
        ui_widget_invalidate(accent_swatches[i]);
    }
}

static void on_accent(ui_widget_t *w) {
    cfg.accent = accents[w->id] ? (0xFF000000u | accents[w->id]) : 0;
    update_accent_swatches();
    apply();
}

static void on_custom_accent(ui_widget_t *w) {
    (void)w;
    uint32_t c = cfg.accent ? cfg.accent : ui_theme.accent;
    if (ui_color_dialog(win, &c)) {
        cfg.accent = c | 0xFF000000u;
        update_accent_swatches();
        apply();
    }
}

static void on_radius(ui_widget_t *w) {
    cfg.radius = w->ival;
    char t[32];
    snprintf(t, sizeof(t), "%d px", w->ival);
    ui_set_text(radius_label, t);
    apply();
}

static void on_shadows(ui_widget_t *w) { cfg.shadows = w->ival; apply(); }
static void on_transp(ui_widget_t *w) { cfg.transparency = w->ival; apply(); }

static void build_appearance(void) {
    building = P_APPEARANCE;
    int y = 64;
    heading(y, "Theme");
    theme_canvas = add(ui_canvas(win, PAGE_X, y + 30, win->w - PAGE_X - 20, 2 * (CARD_H + 30), draw_theme_cards,
                                 theme_cards_event));
    theme_canvas->focusable = false;
    y += 30 + 2 * (CARD_H + 30) + 6;
    heading(y, "Accent color");
    int x = PAGE_X;
    for (int i = 0; i < (int)(sizeof(accents) / sizeof(accents[0])); i++) {
        uint32_t col = accents[i] ? 0xFF000000u | accents[i] : ui_theme.window_text_dim;
        accent_swatches[i] = add(ui_swatch(win, x, y + 28, 28, 28, col, on_accent));
        accent_swatches[i]->id = i;
        x += 36;
    }
    ui_widget_t *def = add(ui_label(win, PAGE_X, y + 58, 60, 18, "Theme"));
    def->font = &ui_font;
    def->color = 0;
    add(ui_button(win, x + 8, y + 26, 110, 32, "Custom...", on_custom_accent));
    y += 90;
    heading(y, "Windows");
    add(ui_label(win, PAGE_X, y + 30, 130, 24, "Corner radius"));
    radius_slider = add(ui_slider(win, PAGE_X + 130, y + 30, 220, 24, 0, 16, cfg.radius >= 0 ? cfg.radius : ui_theme.radius));
    radius_slider->on_activate = on_radius;
    radius_label = add(ui_label(win, PAGE_X + 360, y + 30, 60, 24, ""));
    char t[32];
    snprintf(t, sizeof(t), "%d px", radius_slider->ival);
    ui_set_text(radius_label, t);
    shadows_cb = add(ui_checkbox(win, PAGE_X, y + 62, 200, 26, "Window shadows", cfg.shadows != 0));
    shadows_cb->on_change = on_shadows;
    transp_cb = add(ui_checkbox(win, PAGE_X + 220, y + 62, 220, 26, "Transparent taskbar", cfg.transparency != 0));
    transp_cb->on_change = on_transp;
    update_accent_swatches();
}

/* ------------------------------------------------------------------ background */
#define WPC_W 128
#define WPC_H 80

static void draw_wp_preview(surface_t *s, int style, int x, int y, int w, int h, uint32_t c1, uint32_t c2) {
    surface_t sub = *s;
    rect_t cl;
    if (!rect_intersect(s->clip, mkrect(x, y, w, h), &cl)) return;
    gfx_set_clip(&sub, cl);
    switch (style) {
    case WP_SOLID: gfx_fill(&sub, x, y, w, h, c1); break;
    case WP_WAVES:
        gfx_gradient_v(&sub, x, y, w, h, gfx_darken(c1, 90), c1);
        for (int l = 0; l < 4; l++) {
            uint32_t col = gfx_mix(c1, c2, 70 + l * 60);
            for (int i = 0; i < w; i++) {
                int yy = y + h * (40 + l * 14) / 100 + (int)(4 * ((i * (l + 2) / 9 + l * 3) % 7 - 3) / 3);
                gfx_vline(&sub, x + i, yy, y + h - yy, WITH_ALPHA(col, 170));
            }
        }
        break;
    case WP_AURORA:
        gfx_gradient_v(&sub, x, y, w, h, 0xFF03060F, 0xFF0A1A2C);
        for (int i = 0; i < w; i++) {
            int cy = y + h / 3 + ((i * 7) % 13) - 6;
            gfx_vline(&sub, x + i, cy - 14, 18, WITH_ALPHA(gfx_mix(0xFF38F2A6, c2, i * 255 / w), 110));
        }
        gfx_fill(&sub, x, y + h - 12, w, 12, 0xFF02040A);
        break;
    case WP_MOUNTAINS:
        gfx_gradient_v(&sub, x, y, w, h, c1, gfx_lighten(c2, 60));
        gfx_fill_circle(&sub, x + w * 2 / 3, y + h * 2 / 5, 6, 0xFFFFE8C8);
        gfx_fill_triangle(&sub, x - 10, y + h, x + w / 3, y + h / 3, x + w * 2 / 3, y + h, gfx_mix(c1, 0xFF000000, 60));
        gfx_fill_triangle(&sub, x + w / 3, y + h, x + w * 3 / 4, y + h / 2, x + w + 10, y + h, gfx_mix(c1, 0xFF000000, 140));
        break;
    case WP_BUBBLES:
        gfx_gradient_h(&sub, x, y, w, h, c1, gfx_darken(c2, 60));
        for (int i = 0; i < 6; i++) gfx_fill_circle(&sub, x + (i * 37) % w, y + (i * 23) % h, 6 + i * 2, WITH_ALPHA(c2, 60));
        break;
    case WP_IMAGE:
        gfx_fill(&sub, x, y, w, h, ui_theme.sidebar_bg);
        ui_draw_icon(&sub, "viewer", 32, x + (w - 32) / 2, y + (h - 32) / 2);
        break;
    default: gfx_gradient_h(&sub, x, y, w, h, c1, c2); break;
    }
}

static void draw_wp_cards(ui_widget_t *w, surface_t *s) {
    int cols = MAX(1, w->r.w / (WPC_W + 12));
    for (int i = 0; i < WP_COUNT; i++) {
        int x = w->r.x + (i % cols) * (WPC_W + 12), y = w->r.y + (i / cols) * (WPC_H + 28);
        draw_wp_preview(s, i, x, y, WPC_W, WPC_H, cfg.wp_color1 | 0xFF000000u, cfg.wp_color2 | 0xFF000000u);
        bool sel = (int)cfg.wallpaper == i;
        gfx_rounded_rect(s, x - (sel ? 2 : 0), y - (sel ? 2 : 0), WPC_W + (sel ? 4 : 0), WPC_H + (sel ? 4 : 0), 6,
                         sel ? ui_theme.accent : ui_theme.input_border);
        if (sel) gfx_rounded_rect(s, x - 3, y - 3, WPC_W + 6, WPC_H + 6, 7, ui_theme.accent);
        int tw = font_text_width(&ui_font, wp_names[i]);
        font_draw(s, sel ? &ui_font_bold : &ui_font, x + (WPC_W - tw) / 2, y + WPC_H + 5, wp_names[i], ui_theme.window_text);
    }
}

static bool wp_cards_event(ui_widget_t *w, gui_event_t *ev) {
    if (ev->type != EV_MOUSE_DOWN || ev->button != MOUSE_LEFT) return false;
    int cols = MAX(1, w->r.w / (WPC_W + 12));
    int cx = (ev->x - w->r.x) / (WPC_W + 12), cy = (ev->y - w->r.y) / (WPC_H + 28);
    int i = cy * cols + cx;
    if (cx < cols && i >= 0 && i < WP_COUNT) {
        if (i == WP_IMAGE && !cfg.wp_image[0]) {
            char *p = ui_file_dialog(win, false, "Choose an image", "/home/Pictures", 0, "bmp");
            if (!p) return true;
            strlcpy(cfg.wp_image, p, sizeof(cfg.wp_image));
            free(p);
            ui_set_text(wp_img_label, cfg.wp_image);
        }
        cfg.wallpaper = i;
        apply();
        ui_widget_invalidate(w);
    }
    return true;
}

static void on_wp_color(ui_widget_t *w) {
    uint32_t *c = w == wp_c1 ? &cfg.wp_color1 : &cfg.wp_color2;
    uint32_t v = *c | 0xFF000000u;
    if (ui_color_dialog(win, &v)) {
        *c = v;
        w->color = v;
        ui_widget_invalidate(w);
        ui_widget_invalidate(wp_canvas);
        apply();
    }
}

static void on_wp_preset(ui_widget_t *w) {
    cfg.wp_color1 = 0xFF000000u | wp_presets[w->id].c1;
    cfg.wp_color2 = 0xFF000000u | wp_presets[w->id].c2;
    wp_c1->color = cfg.wp_color1;
    wp_c2->color = cfg.wp_color2;
    ui_widget_invalidate(wp_c1);
    ui_widget_invalidate(wp_c2);
    ui_widget_invalidate(wp_canvas);
    apply();
}

static void on_wp_image(ui_widget_t *w) {
    (void)w;
    char *p = ui_file_dialog(win, false, "Choose an image", "/home/Pictures", 0, "bmp");
    if (!p) return;
    strlcpy(cfg.wp_image, p, sizeof(cfg.wp_image));
    free(p);
    ui_set_text(wp_img_label, cfg.wp_image);
    cfg.wallpaper = WP_IMAGE;
    ui_widget_invalidate(wp_canvas);
    apply();
}

static void on_wp_mode(ui_widget_t *w) { cfg.wp_mode = w->ival; if (cfg.wallpaper == WP_IMAGE) apply(); }

static void draw_preset(ui_widget_t *w, surface_t *s) {
    int i = w->id;
    gfx_fill_rounded(s, w->r.x, w->r.y, w->r.w, w->r.h, 6, 0);
    surface_t sub = *s;
    rect_t cl;
    if (rect_intersect(s->clip, w->r, &cl)) {
        gfx_set_clip(&sub, cl);
        gfx_gradient_h(&sub, w->r.x, w->r.y, w->r.w, w->r.h, 0xFF000000u | wp_presets[i].c1, 0xFF000000u | wp_presets[i].c2);
    }
    gfx_rounded_rect(s, w->r.x, w->r.y, w->r.w, w->r.h, 4, w->win->hover == w ? ui_theme.accent : ui_theme.input_border);
}

static bool preset_event(ui_widget_t *w, gui_event_t *ev) {
    if (ev->type == EV_MOUSE_UP && ev->button == MOUSE_LEFT) on_wp_preset(w);
    if (ev->type == EV_MOUSE_LEAVE || ev->type == EV_MOUSE_MOVE) ui_widget_invalidate(w);
    return true;
}

static void build_background(void) {
    building = P_BACKGROUND;
    int y = 64;
    heading(y, "Wallpaper");
    wp_canvas = add(ui_canvas(win, PAGE_X, y + 30, win->w - PAGE_X - 20, 2 * (WPC_H + 28), draw_wp_cards, wp_cards_event));
    wp_canvas->focusable = false;
    y += 30 + 2 * (WPC_H + 28) + 8;
    heading(y, "Colors");
    add(ui_label(win, PAGE_X, y + 30, 70, 28, "Primary"));
    wp_c1 = add(ui_swatch(win, PAGE_X + 70, y + 30, 44, 28, cfg.wp_color1 | 0xFF000000u, on_wp_color));
    add(ui_label(win, PAGE_X + 130, y + 30, 80, 28, "Secondary"));
    wp_c2 = add(ui_swatch(win, PAGE_X + 210, y + 30, 44, 28, cfg.wp_color2 | 0xFF000000u, on_wp_color));
    add(ui_label(win, PAGE_X, y + 66, 70, 26, "Presets"));
    for (int i = 0; i < (int)(sizeof(wp_presets) / sizeof(wp_presets[0])); i++) {
        ui_widget_t *p = add(ui_canvas(win, PAGE_X + 70 + i * 48, y + 66, 40, 26, draw_preset, preset_event));
        p->id = i;
        p->focusable = false;
        p->cursor = CUR_HAND;
    }
    y += 104;
    heading(y, "Picture");
    wp_img_btn = add(ui_button(win, PAGE_X, y + 28, 140, 32, "Browse...", on_wp_image));
    wp_mode_dd = add(ui_dropdown(win, PAGE_X + 150, y + 28, 120, 32));
    const char *modes[] = { "Fill", "Fit", "Stretch", "Center", "Tile" };
    for (int i = 0; i < 5; i++) ui_dropdown_add(wp_mode_dd, modes[i]);
    ui_dropdown_select(wp_mode_dd, cfg.wp_mode);
    wp_mode_dd->on_change = on_wp_mode;
    wp_img_label = add(ui_label(win, PAGE_X + 282, y + 28, win->w - PAGE_X - 300, 32, cfg.wp_image[0] ? cfg.wp_image : "No picture selected"));
    wp_img_label->color = 0;
}

/* ------------------------------------------------------------------ taskbar */
static void on_tb(ui_widget_t *w) {
    if (w == tb_pos_dd) cfg.taskbar_top = w->ival == 1;
    else if (w == tb_size_dd) cfg.taskbar_size = w->ival;
    else if (w == tb_labels_cb) cfg.taskbar_labels = w->ival;
    else if (w == clk24_cb) cfg.clock_24h = w->ival;
    else if (w == clksec_cb) cfg.clock_seconds = w->ival;
    else if (w == clkdate_cb) cfg.clock_date = w->ival;
    else if (w == icons_cb) cfg.desktop_icons = w->ival;
    apply();
}

static void build_taskbar(void) {
    building = P_TASKBAR;
    int y = 64;
    heading(y, "Taskbar");
    add(ui_label(win, PAGE_X, y + 32, 140, 32, "Position"));
    tb_pos_dd = add(ui_dropdown(win, PAGE_X + 150, y + 32, 160, 32));
    ui_dropdown_add(tb_pos_dd, "Bottom");
    ui_dropdown_add(tb_pos_dd, "Top");
    ui_dropdown_select(tb_pos_dd, cfg.taskbar_top ? 1 : 0);
    tb_pos_dd->on_change = on_tb;
    add(ui_label(win, PAGE_X, y + 72, 140, 32, "Size"));
    tb_size_dd = add(ui_dropdown(win, PAGE_X + 150, y + 72, 160, 32));
    ui_dropdown_add(tb_size_dd, "Small");
    ui_dropdown_add(tb_size_dd, "Normal");
    ui_dropdown_add(tb_size_dd, "Large");
    ui_dropdown_select(tb_size_dd, cfg.taskbar_size);
    tb_size_dd->on_change = on_tb;
    tb_labels_cb = add(ui_checkbox(win, PAGE_X, y + 114, 300, 26, "Show window titles on taskbar buttons", cfg.taskbar_labels));
    tb_labels_cb->on_change = on_tb;
    y += 160;
    heading(y, "Clock");
    clk24_cb = add(ui_checkbox(win, PAGE_X, y + 30, 300, 26, "24-hour clock", cfg.clock_24h));
    clksec_cb = add(ui_checkbox(win, PAGE_X, y + 60, 300, 26, "Show seconds", cfg.clock_seconds));
    clkdate_cb = add(ui_checkbox(win, PAGE_X, y + 90, 300, 26, "Show date", cfg.clock_date));
    clk24_cb->on_change = clksec_cb->on_change = clkdate_cb->on_change = on_tb;
    y += 130;
    heading(y, "Desktop");
    icons_cb = add(ui_checkbox(win, PAGE_X, y + 30, 300, 26, "Show desktop icons", cfg.desktop_icons));
    icons_cb->on_change = on_tb;
}

/* ------------------------------------------------------------------ display */
static void update_res_label(void) {
    gui_screen_t si;
    gui_screen_info(&si);
    char t[128];
    snprintf(t, sizeof(t), "Current resolution: %d x %d", si.width, si.height);
    ui_set_text(res_label, t);
    for (int i = 0; i < NRES; i++)
        if (resolutions[i].w == si.width && resolutions[i].h == si.height) ui_dropdown_select(res_dd, i);
}

static void on_res_apply(ui_widget_t *w) {
    (void)w;
    int i = res_dd->ival;
    if (i < 0 || i >= NRES) return;
    int r =gui_set_resolution(resolutions[i].w, resolutions[i].h);
    if (r < 0) ui_msgbox(win, "Display", "This resolution is not supported by the graphics adapter.", "OK");
    update_res_label();
}

static void build_display(void) {
    building = P_DISPLAY;
    int y = 64;
    gui_screen_t si;
    gui_screen_info(&si);
    heading(y, "Screen resolution");
    res_label = add(ui_label(win, PAGE_X, y + 28, 400, 24, ""));
    res_dd = add(ui_dropdown(win, PAGE_X, y + 60, 200, 32));
    for (int i = 0; i < NRES; i++) {
        char t[32];
        snprintf(t, sizeof(t), "%d x %d", resolutions[i].w, resolutions[i].h);
        ui_dropdown_add(res_dd, t);
    }
    res_apply = add(ui_button(win, PAGE_X + 210, y + 60, 100, 32, "Apply", on_res_apply));
    ui_button_style(res_apply, BTN_PRIMARY);
    if (!si.can_set_mode) {
        ui_enable(res_dd, false);
        ui_enable(res_apply, false);
        note(y + 100, "Changing the resolution at runtime needs a Bochs/QEMU/VirtualBox compatible");
        note(y + 120, "graphics adapter. Choose a resolution in the boot menu instead.");
    } else {
        note(y + 100, "The resolution is applied immediately. You can also pick one in the boot menu.");
    }
    update_res_label();
}

/* ------------------------------------------------------------------ input */
static void on_speed(ui_widget_t *w) { cfg.mouse_speed = w->ival; apply(); }
static void on_dbl(ui_widget_t *w) { cfg.double_click_ms = w->ival; apply(); }
static void on_kbd(ui_widget_t *w) { strlcpy(cfg.kbd_layout, w->ival == 1 ? "us" : "de", sizeof(cfg.kbd_layout)); apply(); }

static void build_input(void) {
    building = P_INPUT;
    int y = 64;
    heading(y, "Mouse");
    add(ui_label(win, PAGE_X, y + 30, 160, 26, "Pointer speed"));
    speed_slider = add(ui_slider(win, PAGE_X + 170, y + 30, 240, 26, 1, 10, cfg.mouse_speed));
    speed_slider->on_activate = on_speed;
    add(ui_label(win, PAGE_X, y + 64, 160, 26, "Double-click speed"));
    dbl_slider = add(ui_slider(win, PAGE_X + 170, y + 64, 240, 26, 200, 900, cfg.double_click_ms));
    dbl_slider->on_activate = on_dbl;
    note(y + 94, "Pointer speed applies to PS/2 mice; in virtual machines the pointer follows the host.");
    y += 140;
    heading(y, "Keyboard");
    add(ui_label(win, PAGE_X, y + 32, 160, 32, "Layout"));
    kbd_dd = add(ui_dropdown(win, PAGE_X + 170, y + 32, 240, 32));
    ui_dropdown_add(kbd_dd, "German (QWERTZ)");
    ui_dropdown_add(kbd_dd, "English (US, QWERTY)");
    ui_dropdown_select(kbd_dd, !strcmp(cfg.kbd_layout, "us") ? 1 : 0);
    kbd_dd->on_change = on_kbd;
    add(ui_label(win, PAGE_X, y + 76, 160, 32, "Test here"));
    ui_widget_t *tb = add(ui_textbox(win, PAGE_X + 170, y + 76, 240, 32, ""));
    ui_textbox_set_placeholder(tb, "Type to test the layout");
}

/* ------------------------------------------------------------------ sound */
static void on_vol(ui_widget_t *w) {
    cfg.volume = w->ival;
    char t[16];
    snprintf(t, sizeof(t), "%d %%", w->ival);
    ui_set_text(vol_label, t);
    apply();
}

static void on_sounds(ui_widget_t *w) { cfg.sounds = w->ival; apply(); }

static void on_test_sound(ui_widget_t *w) {
    (void)w;
    if (syscall1(SYS_SOUND_PLAY, "/system/sounds/notify.wav") < 0) beep(880, 150);
}

static void build_sound(void) {
    building = P_SOUND;
    int y = 64;
    heading(y, "Output");
    add(ui_label(win, PAGE_X, y + 30, 120, 26, "Volume"));
    vol_slider = add(ui_slider(win, PAGE_X + 120, y + 30, 260, 26, 0, 100, cfg.volume));
    vol_slider->on_activate = on_vol;
    vol_label = add(ui_label(win, PAGE_X + 390, y + 30, 60, 26, ""));
    char t[16];
    snprintf(t, sizeof(t), "%u %%", cfg.volume);
    ui_set_text(vol_label, t);
    add(ui_button(win, PAGE_X + 120, y + 66, 160, 32, "Play test sound", on_test_sound));
    sounds_cb = add(ui_checkbox(win, PAGE_X, y + 112, 300, 26, "Play system sounds", cfg.sounds));
    sounds_cb->on_change = on_sounds;
    note(y + 146, "Sound output uses an AC'97 compatible sound card (QEMU: -device AC97).");
}

/* ------------------------------------------------------------------ network */
static void ip_str(uint32_t ip, char *out, size_t n) {
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void refresh_net(void) {
    knetinfo_t ni;
    char buf[600];
    if (syscall1(SYS_NET_INFO, &ni) < 0 || !ni.present) {
        snprintf(buf, sizeof(buf), "No network adapter found.\n\nClaudeOS supports Intel PRO/1000 (e1000) network cards,\nfor example in QEMU (-nic user,model=e1000) or VirtualBox.");
    } else {
        char ip[20], mask[20], gw[20], dns[20], rx[16], tx[16];
        ip_str(ni.ip, ip, sizeof(ip));
        ip_str(ni.netmask, mask, sizeof(mask));
        ip_str(ni.gateway, gw, sizeof(gw));
        ip_str(ni.dns, dns, sizeof(dns));
        format_size(ni.rx_bytes, rx, sizeof(rx));
        format_size(ni.tx_bytes, tx, sizeof(tx));
        snprintf(buf, sizeof(buf),
                 "Adapter: %s\nMAC address: %02x:%02x:%02x:%02x:%02x:%02x\nStatus: %s\n\nIP address: %s\nSubnet mask: %s\nGateway: %s\nDNS server: %s\n\nReceived: %s (%lu packets)\nSent: %s (%lu packets)",
                 ni.driver, ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5],
                 ni.up ? "Connected (DHCP)" : "Waiting for DHCP...", ip, mask, gw, dns, rx,
                 (unsigned long)ni.rx_packets, tx, (unsigned long)ni.tx_packets);
    }
    ui_set_text(net_label, buf);
}

static void on_net_refresh(ui_widget_t *w) { (void)w; refresh_net(); }

static void build_network(void) {
    building = P_NETWORK;
    heading(64, "Ethernet");
    net_label = add(ui_label(win, PAGE_X, 94, win->w - PAGE_X - 24, 280, ""));
    add(ui_button(win, PAGE_X, 380, 120, 32, "Refresh", on_net_refresh));
    refresh_net();
}

/* ------------------------------------------------------------------ date & time */
static void refresh_time(void *arg) {
    (void)arg;
    if (!dt_label) return;
    time_t t = time(0);
    char buf[128];
    strftime(buf, sizeof(buf), "%A, %d %B %Y   %H:%M:%S", localtime(&t));
    if (strcmp(buf, ui_get_text(dt_label))) ui_set_text(dt_label, buf);
}

static void on_set_time(ui_widget_t *w) {
    (void)w;
    int y, mo, d, h, mi, s = 0;
    if (sscanf(ui_get_text(dt_date), "%d-%d-%d", &y, &mo, &d) != 3 ||
        sscanf(ui_get_text(dt_time), "%d:%d:%d", &h, &mi, &s) < 2) {
        ui_msgbox(win, "Date & Time", "Please enter the date as YYYY-MM-DD and the time as HH:MM[:SS].", "OK");
        return;
    }
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    set_time((uint64_t)mktime(&tm));
    refresh_time(0);
}

static void build_datetime(void) {
    building = P_DATETIME;
    heading(64, "Current date and time");
    dt_label = add(ui_label(win, PAGE_X, 94, 500, 30, ""));
    dt_label->font = &ui_font_large;
    heading(150, "Set date and time");
    time_t t = time(0);
    char d[32], tm[32];
    strftime(d, sizeof(d), "%Y-%m-%d", localtime(&t));
    strftime(tm, sizeof(tm), "%H:%M", localtime(&t));
    add(ui_label(win, PAGE_X, 182, 60, 32, "Date"));
    dt_date = add(ui_textbox(win, PAGE_X + 60, 182, 140, 32, d));
    add(ui_label(win, PAGE_X + 220, 182, 50, 32, "Time"));
    dt_time = add(ui_textbox(win, PAGE_X + 270, 182, 100, 32, tm));
    ui_widget_t *b = add(ui_button(win, PAGE_X + 384, 182, 90, 32, "Set", on_set_time));
    ui_button_style(b, BTN_PRIMARY);
    note(226, "The clock is read from the computer's real-time clock (local time).");
    refresh_time(0);
    ui_timer(1000, refresh_time, 0);
}

/* ------------------------------------------------------------------ about */
static void on_reset(ui_widget_t *w) {
    (void)w;
    if (ui_msgbox(win, "Reset settings", "Restore all personalization settings to their defaults?", "Reset|Cancel") != 0) return;
    char kb[8];
    strlcpy(kb, cfg.kbd_layout, sizeof(kb));
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.theme, "Dark", sizeof(cfg.theme));
    cfg.wallpaper = WP_WAVES;
    cfg.wp_color1 = 0xFF2A1B3D;
    cfg.wp_color2 = 0xFFD97757;
    cfg.taskbar_size = 1;
    cfg.taskbar_labels = 1;
    cfg.clock_24h = 1;
    cfg.clock_date = 1;
    cfg.desktop_icons = 1;
    cfg.mouse_speed = 5;
    cfg.double_click_ms = 450;
    strlcpy(cfg.kbd_layout, kb, sizeof(cfg.kbd_layout));
    cfg.volume = 70;
    cfg.sounds = 1;
    cfg.radius = -1;
    cfg.shadows = -1;
    cfg.transparency = 1;
    apply();
    ui_msgbox(win, "Reset settings", "Settings were reset. Reopen Settings to see the new values.", "OK");
}

static void draw_about_logo(ui_widget_t *w, surface_t *s) { ui_draw_icon(s, "logo", 48, w->r.x, w->r.y); }

static void build_about(void) {
    building = P_ABOUT;
    ksysinfo_t si;
    sys_info(&si);
    ui_widget_t *logo = add(ui_canvas(win, PAGE_X, 70, 48, 48, draw_about_logo, 0));
    logo->focusable = false;
    ui_widget_t *name = add(ui_label(win, PAGE_X + 64, 70, 300, 30, "ClaudeOS 1.0"));
    name->font = &ui_font_title;
    ui_widget_t *sub = add(ui_label(win, PAGE_X + 64, 100, 400, 20, "A 64-bit hobby operating system written in C"));
    sub->color = 0;
    char buf[600], mem[16];
    format_size(si.mem_total, mem, sizeof(mem));
    snprintf(buf, sizeof(buf), "Processor: %s\nMemory: %s\nScreen: %u x %u\nBoot loader: %s\n\nKernel: preemptive multitasking, virtual memory,\nVFS with RAM and FAT32 file systems, window server.",
             si.cpu_brand, mem, si.screen_w, si.screen_h, si.bootloader);
    add(ui_label(win, PAGE_X, 140, win->w - PAGE_X - 24, 150, buf));
    add(ui_button(win, PAGE_X, 320, 200, 32, "Reset all settings...", on_reset));
}

/* ------------------------------------------------------------------ main */
static void paint(ui_window_t *w, surface_t *s) {
    gfx_fill(s, 0, 0, SIDE_W, w->h, ui_theme.sidebar_bg);
    gfx_vline(s, SIDE_W, 0, w->h, ui_theme.input_border);
    ui_draw_icon(s, "settings", 32, 16, 14);
    font_draw(s, &ui_font_title, 58, 18, "Settings", ui_theme.window_text);
}

static void on_event(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->type == EV_THEME) {
        /* the theme changed (maybe by us): refresh the theme previews */
        gui_config_get(&cfg);
        ui_invalidate(win);
    }
}

int main(int argc, char **argv) {
    ui_init();
    gui_config_get(&cfg);
    for (npresets = 0; npresets < 16 && gui_theme_get(&presets[npresets], npresets + 1) == 0; npresets++) {}
    win = ui_window("Settings", 900, 600, 0, "settings");
    if (!win) return 1;
    win->on_paint = paint;
    win->on_event = on_event;
    sidebar = ui_list(win, 10, 64, SIDE_W - 20, P_COUNT * 36 + 4);
    ui_list_set_row_height(sidebar, 36);
    for (int i = 0; i < P_COUNT; i++) ui_list_add(sidebar, page_names[i], page_icons[i], 0);
    sidebar->on_change = on_sidebar;
    title_label = ui_label(win, PAGE_X, 16, 400, 34, "");
    title_label->font = &ui_font_title;
    build_appearance();
    build_background();
    build_taskbar();
    build_display();
    build_input();
    build_sound();
    build_network();
    build_datetime();
    build_about();
    int start = P_APPEARANCE;
    if (argc > 1) {
        if (!strcmp(argv[1], "background")) start = P_BACKGROUND;
        else if (!strcmp(argv[1], "display")) start = P_DISPLAY;
        else if (!strcmp(argv[1], "taskbar")) start = P_TASKBAR;
        else if (!strcmp(argv[1], "sound")) start = P_SOUND;
        else if (!strcmp(argv[1], "network")) start = P_NETWORK;
    }
    ui_list_select(sidebar, start);
    show_page(start);
    ui_focus(win, sidebar);
    ui_run();
    return 0;
}
