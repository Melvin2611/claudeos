/* Desktop shell drawn by the window server: desktop icons, taskbar, start menu,
 * tray popups (calendar, volume, network, power), context menu, alt-tab, notifications */
#include "wm.h"
#include <vfs.h>
#include <proc.h>

/* ------------------------------------------------------------------ app registry */
#define MAX_APPS 48
typedef struct {
    char name[40];
    char exec[64];
    char arg[64];
    char icon[32];
    bool desktop;
    char category[24];
} app_t;
static app_t apps[MAX_APPS];
static int napps;

static void load_apps(void) {
    napps = 0;
    size_t size;
    char *data = vfs_read_all("/etc/apps.conf", &size);
    if (!data) return;
    char *line = data;
    while (line && *line && napps < MAX_APPS) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (*line && *line != '#') {
            char *f[6] = { 0 };
            int nf = 0;
            char *p = line;
            while (nf < 6) {
                f[nf++] = p;
                char *bar = strchr(p, '|');
                if (!bar) break;
                *bar = 0;
                p = bar + 1;
            }
            if (nf >= 3) {
                app_t *a = &apps[napps++];
                memset(a, 0, sizeof(*a));
                strlcpy(a->name, f[0], sizeof(a->name));
                strlcpy(a->exec, f[1], sizeof(a->exec));
                strlcpy(a->icon, f[2], sizeof(a->icon));
                a->desktop = nf > 3 && f[3][0] == '1';
                if (nf > 4) strlcpy(a->category, f[4], sizeof(a->category));
                if (nf > 5) strlcpy(a->arg, f[5], sizeof(a->arg));
            }
        }
        line = nl ? nl + 1 : 0;
    }
    kfree(data);
}

static void launch_app(app_t *a) { wm_launch(a->exec, a->arg[0] ? a->arg : 0); }

/* ------------------------------------------------------------------ state */
enum { OV_NONE, OV_START, OV_CALENDAR, OV_VOLUME, OV_NETWORK, OV_MENU };
static int overlay;
static rect_t ov_rect;

/* start menu */
static char search[48];
static int start_sel = -1;
static int start_hover = -1;
static int start_scroll;
static bool power_hover, power_menu_from_start;

/* generic server-side menu (desktop context menu, power menu) */
#define MENU_MAX 10
typedef struct { const char *label; const char *glyph; int action; } menu_item_t;
static menu_item_t menu_items[MENU_MAX];
static int menu_count, menu_hover = -1;

enum { ACT_TERMINAL = 1, ACT_FILES, ACT_WALLPAPER, ACT_PERSONALIZE, ACT_DISPLAY, ACT_SHUTDOWN, ACT_RESTART,
       ACT_TASKMGR, ACT_ABOUT, ACT_SETTINGS };

/* taskbar */
#define TB_MAX_BUTTONS 24
static struct { window_t *w; rect_t r; int id; } tb_buttons[TB_MAX_BUTTONS];
static int tb_nbuttons;
static int tb_hover = -1;       /* -2 start, -3 clock, -4 volume, -5 network, -6 show desktop */
static char clock_str[32], date_str[32];

/* desktop icons */
typedef struct { app_t *app; rect_t r; } dicon_t;
static dicon_t dicons[MAX_APPS];
static int ndicons, dicon_sel = -1;

/* calendar */
static int cal_month, cal_year;   /* shown month */

/* volume */
static bool vol_dragging;

/* alt-tab */
static bool alttab_active;
static int alttab_index;
static window_t *alttab_list[32];
static int alttab_count;

/* notifications */
typedef struct { char title[64]; char text[128]; char icon[32]; uint64_t until; bool used; } toast_t;
static toast_t toasts[3];

/* shutdown */
static const char *power_msg;

/* ------------------------------------------------------------------ helpers */

static int tb_h(void) { return wm.cfg.taskbar_size == 0 ? 36 : wm.cfg.taskbar_size == 2 ? 52 : 44; }
int shell_taskbar_height(void) { return tb_h(); }
rect_t shell_taskbar_rect(void) {
    return wm.cfg.taskbar_top ? mkrect(0, 0, wm.w, tb_h()) : mkrect(0, wm.h - tb_h(), wm.w, tb_h());
}

static bool retro(void) { return wm.theme.style == 1; }

static void bevel_rect(surface_t *s, rect_t r, bool sunken) {
    uint32_t hi = 0xFFFFFFFF, dk = 0xFF000000, lo = 0xFF808080;
    if (sunken) { hi = 0xFF000000; dk = 0xFFFFFFFF; lo = 0xFFDFDFDF; }
    gfx_hline(s, r.x, r.y, r.w, hi);
    gfx_vline(s, r.x, r.y, r.h, hi);
    gfx_hline(s, r.x, r.y + r.h - 1, r.w, dk);
    gfx_vline(s, r.x + r.w - 1, r.y, r.h, dk);
    gfx_hline(s, r.x + 1, r.y + r.h - 2, r.w - 2, lo);
    gfx_vline(s, r.x + r.w - 2, r.y + 1, r.h - 2, lo);
}

/* panel background used by all popups */
static void panel(surface_t *s, rect_t r) {
    if (retro()) {
        gfx_fill(s, r.x, r.y, r.w, r.h, wm.theme.menu_bg);
        bevel_rect(s, r, false);
        return;
    }
    int rad = MIN(wm.theme.radius + 4, 14);
    if (wm.theme.shadows) gfx_shadow(s, r, rad, 20, 110, 6);
    gfx_fill_rounded(s, r.x, r.y, r.w, r.h, rad, wm.theme.menu_bg);
    gfx_rounded_rect(s, r.x, r.y, r.w, r.h, rad, wm.theme.menu_border);
}

static void damage_overlay(void) { if (overlay) wm_damage(mkrect(ov_rect.x - 30, ov_rect.y - 30, ov_rect.w + 60, ov_rect.h + 66)); }

static void close_overlay(void) {
    damage_overlay();
    overlay = OV_NONE;
    power_menu_from_start = false;
    vol_dragging = false;
    wm_damage(shell_taskbar_rect());
}

bool shell_overlay_open(void) { return overlay != OV_NONE; }
void shell_close_overlays(void) { close_overlay(); }

static void open_overlay(int kind, rect_t r) {
    damage_overlay();
    overlay = kind;
    ov_rect = r;
    damage_overlay();
    wm_damage(shell_taskbar_rect());
}

/* position a popup of size w x h next to the taskbar, horizontally anchored at ax */
static rect_t popup_rect_at(int ax, int w, int h) {
    rect_t tb = shell_taskbar_rect();
    int x = MAX(8, MIN(ax, wm.w - w - 8));
    int y = wm.cfg.taskbar_top ? tb.y + tb.h + 8 : tb.y - h - 8;
    return mkrect(x, y, w, h);
}

/* ------------------------------------------------------------------ desktop */

void shell_desktop_reload(void) {
    ndicons = 0;
    dicon_sel = -1;
    if (!wm.cfg.desktop_icons) { wm_damage_all(); return; }
    rect_t wa = wm_work_area();
    int cw = 96, ch = 94;
    int col = 0, row = 0;
    int rows = MAX(1, (wa.h - 16) / ch);
    for (int i = 0; i < napps; i++) {
        if (!apps[i].desktop) continue;
        dicons[ndicons].app = &apps[i];
        dicons[ndicons].r = mkrect(wa.x + 10 + col * cw, wa.y + 12 + row * ch, cw - 8, ch - 6);
        ndicons++;
        if (++row >= rows) { row = 0; col++; }
    }
    wm_damage_all();
}

static void draw_text_shadowed(surface_t *s, const font_t *f, int x, int y, const char *t, uint32_t c) {
    font_draw(s, f, x + 1, y + 1, t, 0xB0000000);
    font_draw(s, f, x, y, t, c);
}

static void draw_desktop(surface_t *s, rect_t clip) {
    for (int i = 0; i < ndicons; i++) {
        dicon_t *d = &dicons[i];
        if (!rect_intersect(d->r, clip, 0)) continue;
        if (i == dicon_sel) {
            gfx_fill_rounded(s, d->r.x, d->r.y, d->r.w, d->r.h, 8, WITH_ALPHA(wm.theme.accent, 90));
            gfx_rounded_rect(s, d->r.x, d->r.y, d->r.w, d->r.h, 8, WITH_ALPHA(wm.theme.accent, 180));
        }
        icon_t *ic = icon_get(d->app->icon, 48);
        if (ic) draw_icon(s, ic, d->r.x + (d->r.w - 48) / 2, d->r.y + 6);
        /* label, wrapped onto up to two lines */
        const char *name = d->app->name;
        int maxw = d->r.w - 6;
        int tw = font_text_width(F_UI, name);
        if (tw <= maxw) {
            draw_text_shadowed(s, F_UI, d->r.x + (d->r.w - tw) / 2, d->r.y + 58, name, 0xFFFFFFFF);
        } else {
            const char *sp = strrchr(name, ' ');
            char a[40];
            if (sp) {
                int n = (int)(sp - name);
                strlcpy(a, name, MIN((size_t)n + 1, sizeof(a)));
                int w1 = font_text_width(F_UI, a), w2 = font_text_width(F_UI, sp + 1);
                draw_text_shadowed(s, F_UI, d->r.x + (d->r.w - w1) / 2, d->r.y + 56, a, 0xFFFFFFFF);
                draw_text_shadowed(s, F_UI, d->r.x + (d->r.w - w2) / 2, d->r.y + 56 + F_UI->height, sp + 1, 0xFFFFFFFF);
            } else {
                font_draw_fit(s, F_UI, d->r.x + 3, d->r.y + 58, name, maxw, 0xFFFFFFFF);
            }
        }
    }
}

static int dicon_at(int x, int y) {
    for (int i = 0; i < ndicons; i++)
        if (rect_contains(dicons[i].r, x, y)) return i;
    return -1;
}

/* ------------------------------------------------------------------ taskbar */

static void build_tb_buttons(void) {
    tb_nbuttons = 0;
    for (window_t *w = wm.windows; w && tb_nbuttons < TB_MAX_BUTTONS; w = w->next) {
        if ((w->flags & (WF_NO_TASKBAR | WF_POPUP)) || (!w->visible && !w->minimized)) continue;
        if (w->parent && (w->flags & WF_DIALOG)) continue;
        tb_buttons[tb_nbuttons].w = w;
        tb_buttons[tb_nbuttons].id = w->id;
        tb_nbuttons++;
    }
}

static int tray_width(void) {
    int cw = wm.cfg.clock_date ? 86 : 64;
    if (wm.cfg.clock_seconds) cw += 16;
    if (!wm.cfg.clock_24h) cw += 18;
    return cw + 2 * 34 + 12;
}

static rect_t start_btn_rect(void) {
    rect_t tb = shell_taskbar_rect();
    int sz = tb.h - 8;
    if (retro()) return mkrect(tb.x + 3, tb.y + 4, 64, tb.h - 8);
    return mkrect(tb.x + 6, tb.y + 4, sz + 6, sz);
}

static void layout_taskbar(void) {
    build_tb_buttons();
    rect_t tb = shell_taskbar_rect();
    rect_t sb = start_btn_rect();
    int x0 = sb.x + sb.w + 8;
    int x1 = tb.w - tray_width() - 8;
    int n = tb_nbuttons;
    if (!n) return;
    int bw;
    if (wm.cfg.taskbar_labels) bw = MIN(190, (x1 - x0) / n - 4);
    else bw = tb.h - 4;
    bw = MAX(bw, 36);
    for (int i = 0; i < n; i++) tb_buttons[i].r = mkrect(x0 + i * (bw + 4), tb.y + 4, bw, tb.h - 8);
}

static rect_t clock_rect(void) {
    rect_t tb = shell_taskbar_rect();
    int cw = tray_width() - 2 * 34 - 12;
    return mkrect(tb.w - cw - 12, tb.y + 4, cw, tb.h - 8);
}
static rect_t volume_rect(void) { rect_t c = clock_rect(); return mkrect(c.x - 34, c.y, 32, c.h); }
static rect_t network_rect(void) { rect_t v = volume_rect(); return mkrect(v.x - 34, v.y, 32, v.h); }
static rect_t showdesk_rect(void) { rect_t tb = shell_taskbar_rect(); return mkrect(tb.w - 8, tb.y, 8, tb.h); }

extern bool net_status(char *ip, size_t n);
__attribute__((weak)) bool net_status(char *ip, size_t n) { if (n) ip[0] = 0; return false; }

static void update_clock(bool force) {
    uint64_t t = time_now();
    uint64_t days = t / 86400, secs = t % 86400;
    int h = (int)(secs / 3600), m = (int)(secs / 60 % 60), sc = (int)(secs % 60);
    char c[32], d[32];
    if (wm.cfg.clock_24h) {
        if (wm.cfg.clock_seconds) snprintf(c, sizeof(c), "%02d:%02d:%02d", h, m, sc);
        else snprintf(c, sizeof(c), "%02d:%02d", h, m);
    } else {
        int h12 = h % 12 ? h % 12 : 12;
        if (wm.cfg.clock_seconds) snprintf(c, sizeof(c), "%d:%02d:%02d %s", h12, m, sc, h < 12 ? "AM" : "PM");
        else snprintf(c, sizeof(c), "%d:%02d %s", h12, m, h < 12 ? "AM" : "PM");
    }
    /* civil date */
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153, dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9;
    y += mm <= 2;
    snprintf(d, sizeof(d), "%02d.%02d.%04d", (int)dd, (int)mm, (int)y);
    if (force || strcmp(c, clock_str) || strcmp(d, date_str)) {
        strlcpy(clock_str, c, sizeof(clock_str));
        strlcpy(date_str, d, sizeof(date_str));
        wm_damage(clock_rect());
        if (overlay == OV_CALENDAR) damage_overlay();
    }
}

static void tb_item_bg(surface_t *s, rect_t r, bool hover, bool active) {
    if (retro()) return;
    if (active) gfx_fill_rounded(s, r.x, r.y, r.w, r.h, 6, WITH_ALPHA(wm.theme.panel_active, 200));
    else if (hover) gfx_fill_rounded(s, r.x, r.y, r.w, r.h, 6, WITH_ALPHA(wm.theme.panel_hover, 200));
}

static void draw_taskbar(surface_t *s, rect_t clip) {
    rect_t tb = shell_taskbar_rect();
    if (!rect_intersect(tb, clip, 0)) return;
    layout_taskbar();
    if (retro()) {
        gfx_fill(s, tb.x, tb.y, tb.w, tb.h, wm.theme.panel_bg);
        gfx_hline(s, tb.x, wm.cfg.taskbar_top ? tb.y + tb.h - 1 : tb.y + 1, tb.w, 0xFFFFFFFF);
    } else {
        gfx_fill(s, tb.x, tb.y, tb.w, tb.h, WITH_ALPHA(wm.theme.panel_bg, wm.theme.panel_alpha));
        gfx_hline(s, tb.x, wm.cfg.taskbar_top ? tb.y + tb.h - 1 : tb.y, tb.w,
                  wm.theme.dark ? 0x30FFFFFF : 0x28000000);
    }
    /* start button */
    rect_t sb = start_btn_rect();
    if (retro()) {
        gfx_fill(s, sb.x, sb.y, sb.w, sb.h, wm.theme.panel_bg);
        bevel_rect(s, sb, overlay == OV_START);
        icon_t *ic = icon_get("logo", 16);
        draw_icon(s, ic, sb.x + 5, sb.y + (sb.h - 16) / 2);
        font_draw(s, F_UIB, sb.x + 25, sb.y + (sb.h - F_UIB->height) / 2, "Start", 0xFF000000);
    } else {
        tb_item_bg(s, sb, tb_hover == -2, overlay == OV_START);
        int isz = tb.h >= 44 ? 24 : 16;
        icon_t *ic = icon_get("logo", isz);
        draw_icon(s, ic, sb.x + (sb.w - isz) / 2, sb.y + (sb.h - isz) / 2);
    }
    /* window buttons */
    for (int i = 0; i < tb_nbuttons; i++) {
        window_t *w = tb_buttons[i].w;
        rect_t r = tb_buttons[i].r;
        bool active = wm.focused == w && !w->minimized;
        if (retro()) {
            gfx_fill(s, r.x, r.y, r.w, r.h, active ? 0xFFE0E0E0 : wm.theme.panel_bg);
            bevel_rect(s, r, active);
        } else {
            tb_item_bg(s, r, tb_hover == i, active);
        }
        int isz = (tb.h >= 44 && !retro()) ? 24 : 16;
        icon_t *ic = icon_get(w->icon[0] ? w->icon : "file-exec", isz);
        int ix = wm.cfg.taskbar_labels ? r.x + 8 : r.x + (r.w - isz) / 2;
        draw_icon(s, ic, ix, r.y + (r.h - isz) / 2);
        if (wm.cfg.taskbar_labels && r.w > 60)
            font_draw_fit(s, active && retro() ? F_UIB : F_UI, ix + isz + 8, r.y + (r.h - F_UI->height) / 2, w->title,
                          r.w - isz - 24, wm.theme.panel_text);
        if (!retro()) {
            /* indicator bar */
            int iw = active ? MIN(r.w - 16, 28) : 8;
            int iy = wm.cfg.taskbar_top ? r.y + 1 : r.y + r.h - 3;
            gfx_fill_rounded(s, r.x + (r.w - iw) / 2, iy, iw, 3, 1,
                             active ? wm.theme.accent : WITH_ALPHA(wm.theme.panel_text, 110));
        }
    }
    /* tray */
    rect_t nr = network_rect(), vr = volume_rect(), cr = clock_rect();
    if (retro()) {
        rect_t tray = mkrect(nr.x - 4, cr.y, cr.x + cr.w - nr.x + 6, cr.h);
        bevel_rect(s, tray, true);
    }
    tb_item_bg(s, nr, tb_hover == -5, overlay == OV_NETWORK);
    tb_item_bg(s, vr, tb_hover == -4, overlay == OV_VOLUME);
    tb_item_bg(s, cr, tb_hover == -3, overlay == OV_CALENDAR);
    char ip[32];
    bool online = net_status(ip, sizeof(ip));
    draw_glyph(s, online ? "net" : "net-off", 16, nr.x + 8, nr.y + (nr.h - 16) / 2, wm.theme.panel_text);
    const char *vg = wm.cfg.volume == 0 ? "volume-mute" : wm.cfg.volume < 50 ? "volume-low" : "volume";
    draw_glyph(s, vg, 16, vr.x + 8, vr.y + (vr.h - 16) / 2, wm.theme.panel_text);
    if (wm.cfg.clock_date && tb.h >= 40) {
        int tw = font_text_width(F_UI, clock_str), dw = font_text_width(F_UI, date_str);
        int total = F_UI->height * 2;
        int y0 = cr.y + (cr.h - total) / 2;
        font_draw(s, F_UI, cr.x + (cr.w - tw) / 2, y0, clock_str, wm.theme.panel_text);
        font_draw(s, F_UI, cr.x + (cr.w - dw) / 2, y0 + F_UI->height, date_str, wm.theme.panel_text);
    } else {
        int tw = font_text_width(F_UI, clock_str);
        font_draw(s, F_UI, cr.x + (cr.w - tw) / 2, cr.y + (cr.h - F_UI->height) / 2, clock_str, wm.theme.panel_text);
    }
    if (!retro()) {
        rect_t sd = showdesk_rect();
        gfx_vline(s, sd.x, sd.y + 8, sd.h - 16, WITH_ALPHA(wm.theme.panel_text, tb_hover == -6 ? 160 : 60));
    }
}

/* ------------------------------------------------------------------ start menu */

#define SM_W 420
#define SM_COLS 4
#define TILE_W 94
#define TILE_H 86

static int start_matches(int *idx, int max) {
    int n = 0;
    for (int i = 0; i < napps && n < max; i++) {
        if (search[0]) {
            /* case-insensitive substring match on name or category */
            bool hit = false;
            size_t sl = strlen(search);
            for (const char *p = apps[i].name; *p && !hit; p++)
                if (!strncasecmp(p, search, sl)) hit = true;
            for (const char *p = apps[i].category; *p && !hit; p++)
                if (!strncasecmp(p, search, sl)) hit = true;
            if (!hit) continue;
        }
        idx[n++] = i;
    }
    return n;
}

static rect_t start_rect(void) {
    int rows = (napps + SM_COLS - 1) / SM_COLS;
    int h = 64 + 30 + MIN(rows, 5) * TILE_H + 16 + 60;
    h = MIN(h, wm.h - tb_h() - 24);
    rect_t sb = start_btn_rect();
    return popup_rect_at(retro() ? 2 : sb.x, SM_W, h);
}

static rect_t tile_rect(rect_t m, int k) {
    int gx = m.x + (m.w - SM_COLS * TILE_W) / 2;
    int gy = m.y + 64 + 30;
    return mkrect(gx + (k % SM_COLS) * TILE_W, gy + (k / SM_COLS) * TILE_H - start_scroll, TILE_W - 4, TILE_H - 4);
}

static rect_t power_btn_rect(rect_t m) { return mkrect(m.x + m.w - 52, m.y + m.h - 50, 40, 40); }

static void draw_start(surface_t *s) {
    rect_t m = ov_rect;
    panel(s, m);
    /* search box */
    rect_t sb = mkrect(m.x + 16, m.y + 16, m.w - 32, 34);
    if (retro()) {
        gfx_fill(s, sb.x, sb.y, sb.w, sb.h, 0xFFFFFFFF);
        bevel_rect(s, sb, true);
    } else {
        gfx_fill_rounded(s, sb.x, sb.y, sb.w, sb.h, 17, wm.theme.input_bg);
        gfx_rounded_rect(s, sb.x, sb.y, sb.w, sb.h, 17, search[0] ? wm.theme.accent : wm.theme.input_border);
    }
    draw_glyph(s, "search", 16, sb.x + 12, sb.y + 9, wm.theme.window_text_dim);
    if (search[0]) {
        int ex = font_draw(s, F_UI, sb.x + 38, sb.y + (sb.h - F_UI->height) / 2, search, wm.theme.input_text);
        gfx_vline(s, ex + 1, sb.y + 8, sb.h - 16, wm.theme.accent);
    } else {
        font_draw(s, F_UI, sb.x + 38, sb.y + (sb.h - F_UI->height) / 2, "Type to search apps", wm.theme.window_text_dim);
    }
    font_draw(s, F_UIB, m.x + 24, m.y + 64 + 4, search[0] ? "Results" : "All apps", wm.theme.menu_text);
    /* tiles */
    int idx[MAX_APPS];
    int n = start_matches(idx, MAX_APPS);
    rect_t area = mkrect(m.x, m.y + 64 + 26, m.w, m.h - 64 - 26 - 62);
    surface_t sub = *s;
    rect_t cl;
    if (rect_intersect(s->clip, area, &cl)) {
        gfx_set_clip(&sub, cl);
        for (int k = 0; k < n; k++) {
            rect_t t = tile_rect(m, k);
            if (!rect_intersect(t, area, 0)) continue;
            app_t *a = &apps[idx[k]];
            bool hl = k == start_hover || k == start_sel;
            if (hl) {
                if (retro()) gfx_fill(&sub, t.x, t.y, t.w, t.h, wm.theme.menu_hover);
                else gfx_fill_rounded(&sub, t.x, t.y, t.w, t.h, 8, wm.theme.menu_hover);
            }
            icon_t *ic = icon_get(a->icon, 32);
            draw_icon(&sub, ic, t.x + (t.w - 32) / 2, t.y + 12);
            uint32_t tc = (hl && retro()) ? 0xFFFFFFFF : wm.theme.menu_text;
            int tw = font_text_width(F_UI, a->name);
            if (tw <= t.w - 6) font_draw(&sub, F_UI, t.x + (t.w - tw) / 2, t.y + 52, a->name, tc);
            else font_draw_fit(&sub, F_UI, t.x + 3, t.y + 52, a->name, t.w - 6, tc);
        }
        if (n == 0) font_draw(&sub, F_UI, m.x + 24, area.y + 20, "No apps found.", wm.theme.window_text_dim);
    }
    /* bottom bar */
    int by = m.y + m.h - 60;
    gfx_hline(s, m.x + 1, by, m.w - 2, wm.theme.menu_border);
    draw_glyph(s, "user", 20, m.x + 24, by + 20, wm.theme.menu_text);
    font_draw(s, F_UIB, m.x + 54, by + 13, "User", wm.theme.menu_text);
    font_draw(s, F_UI, m.x + 54, by + 13 + F_UIB->height, "ClaudeOS 1.0", wm.theme.window_text_dim);
    rect_t pb = power_btn_rect(m);
    if (power_hover || power_menu_from_start) gfx_fill_rounded(s, pb.x, pb.y, pb.w, pb.h, 8, wm.theme.menu_hover);
    draw_glyph(s, "power", 20, pb.x + 10, pb.y + 10, wm.theme.menu_text);
}

static void open_start(void) {
    search[0] = 0;
    start_sel = -1;
    start_hover = -1;
    start_scroll = 0;
    load_apps();
    open_overlay(OV_START, start_rect());
}

/* ------------------------------------------------------------------ menus */

static rect_t menu_rect_at(int x, int y) {
    int w = 220, h = 8;
    for (int i = 0; i < menu_count; i++) h += menu_items[i].label ? 32 : 9;
    x = MIN(x, wm.w - w - 4);
    y = MIN(y, wm.h - h - 4);
    return mkrect(MAX(4, x), MAX(4, y), w, h);
}

static int menu_item_at(int x, int y) {
    if (!rect_contains(ov_rect, x, y)) return -1;
    int yy = ov_rect.y + 4;
    for (int i = 0; i < menu_count; i++) {
        int h = menu_items[i].label ? 32 : 9;
        if (y >= yy && y < yy + h) return menu_items[i].label ? i : -1;
        yy += h;
    }
    return -1;
}

static void draw_menu(surface_t *s) {
    panel(s, ov_rect);
    int yy = ov_rect.y + 4;
    for (int i = 0; i < menu_count; i++) {
        if (!menu_items[i].label) {
            gfx_hline(s, ov_rect.x + 8, yy + 4, ov_rect.w - 16, wm.theme.menu_border);
            yy += 9;
            continue;
        }
        rect_t r = mkrect(ov_rect.x + 4, yy, ov_rect.w - 8, 32);
        bool h = i == menu_hover;
        if (h) {
            if (retro()) gfx_fill(s, r.x, r.y, r.w, r.h, wm.theme.menu_hover);
            else gfx_fill_rounded(s, r.x, r.y, r.w, r.h, 6, wm.theme.menu_hover);
        }
        uint32_t tc = (h && retro()) ? 0xFFFFFFFF : wm.theme.menu_text;
        if (menu_items[i].glyph) draw_glyph(s, menu_items[i].glyph, 16, r.x + 10, r.y + 8, tc);
        font_draw(s, F_UI, r.x + 36, r.y + (32 - F_UI->height) / 2, menu_items[i].label, tc);
        yy += 32;
    }
}

static void add_menu(const char *label, const char *glyph, int action) {
    if (menu_count < MENU_MAX) menu_items[menu_count++] = (menu_item_t){ label, glyph, action };
}

static void open_menu(int x, int y) {
    menu_hover = -1;
    open_overlay(OV_MENU, menu_rect_at(x, y));
}

static void do_power(int action);

static void run_action(int a) {
    switch (a) {
    case ACT_TERMINAL: wm_launch("/bin/terminal", 0); break;
    case ACT_FILES: wm_launch("/bin/files", 0); break;
    case ACT_WALLPAPER: wm_launch("/bin/settings", "background"); break;
    case ACT_PERSONALIZE: wm_launch("/bin/settings", "appearance"); break;
    case ACT_DISPLAY: wm_launch("/bin/settings", "display"); break;
    case ACT_TASKMGR: wm_launch("/bin/taskmgr", 0); break;
    case ACT_ABOUT: wm_launch("/bin/about", 0); break;
    case ACT_SETTINGS: wm_launch("/bin/settings", 0); break;
    case ACT_SHUTDOWN: case ACT_RESTART: do_power(a); break;
    default:
        if (a >= 100 && a - 100 < ndicons) launch_app(dicons[a - 100].app);
        break;
    }
}

/* ------------------------------------------------------------------ calendar */

static int days_in_month(int m, int y) {
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}

static int weekday(int y, int m, int d) {   /* 0 = Monday */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   /* 0 = Sunday */
    return (w + 6) % 7;
}

static void today(int *y, int *m, int *d) {
    int dd, mm, yy;
    /* parse date_str (dd.mm.yyyy) */
    dd = atoi(date_str);
    mm = atoi(date_str + 3);
    yy = atoi(date_str + 6);
    *y = yy; *m = mm; *d = dd;
}

static const char *month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };

static rect_t cal_nav_rect(int dir) {
    return mkrect(ov_rect.x + ov_rect.w - (dir < 0 ? 80 : 44), ov_rect.y + 96, 32, 28);
}

static void draw_calendar(surface_t *s) {
    rect_t r = ov_rect;
    panel(s, r);
    font_draw(s, F_DISPLAY, r.x + 20, r.y + 14, clock_str, wm.theme.menu_text);
    int ty, tm, td;
    today(&ty, &tm, &td);
    static const char *wd_long[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
    char line[64];
    snprintf(line, sizeof(line), "%s, %d %s %d", wd_long[weekday(ty, tm, td)], td, month_names[tm - 1], ty);
    font_draw(s, F_UI, r.x + 22, r.y + 62, line, wm.theme.window_text_dim);
    gfx_hline(s, r.x + 12, r.y + 88, r.w - 24, wm.theme.menu_border);
    snprintf(line, sizeof(line), "%s %d", month_names[cal_month - 1], cal_year);
    font_draw(s, F_UIB, r.x + 22, r.y + 100, line, wm.theme.menu_text);
    rect_t pv = cal_nav_rect(-1), nx = cal_nav_rect(1);
    draw_glyph(s, "chev-up", 16, pv.x + 8, pv.y + 6, wm.theme.menu_text);
    draw_glyph(s, "chev-down", 16, nx.x + 8, nx.y + 6, wm.theme.menu_text);
    static const char *wd[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
    int cw = (r.w - 24) / 7;
    int gx = r.x + 12, gy = r.y + 134;
    for (int i = 0; i < 7; i++) {
        int tw = font_text_width(F_UI, wd[i]);
        font_draw(s, F_UI, gx + i * cw + (cw - tw) / 2, gy, wd[i], wm.theme.window_text_dim);
    }
    int first = weekday(cal_year, cal_month, 1);
    int nd = days_in_month(cal_month, cal_year);
    for (int d = 1; d <= nd; d++) {
        int cell = first + d - 1;
        int cx = gx + (cell % 7) * cw, cy = gy + 26 + (cell / 7) * 32;
        char num[4];
        snprintf(num, sizeof(num), "%d", d);
        int tw = font_text_width(F_UI, num);
        bool is_today = d == td && cal_month == tm && cal_year == ty;
        if (is_today) gfx_fill_circle(s, cx + cw / 2, cy + F_UI->height / 2, 14, wm.theme.accent);
        font_draw(s, is_today ? F_UIB : F_UI, cx + (cw - tw) / 2, cy, num,
                  is_today ? wm.theme.accent_text : (cell % 7 >= 5 ? wm.theme.window_text_dim : wm.theme.menu_text));
    }
}

/* ------------------------------------------------------------------ volume & network */

extern void snd_set_volume(int v);
__attribute__((weak)) void snd_set_volume(int v) { UNUSED(v); }

static rect_t vol_slider(void) { return mkrect(ov_rect.x + 52, ov_rect.y + 44, ov_rect.w - 110, 20); }

static void draw_volume(surface_t *s) {
    rect_t r = ov_rect;
    panel(s, r);
    font_draw(s, F_UIB, r.x + 16, r.y + 12, "Volume", wm.theme.menu_text);
    const char *vg = wm.cfg.volume == 0 ? "volume-mute" : wm.cfg.volume < 50 ? "volume-low" : "volume";
    draw_glyph(s, vg, 20, r.x + 18, r.y + 44, wm.theme.menu_text);
    rect_t sl = vol_slider();
    int fillw = sl.w * (int)wm.cfg.volume / 100;
    gfx_fill_rounded(s, sl.x, sl.y + 8, sl.w, 4, 2, wm.theme.scroll_track);
    gfx_fill_rounded(s, sl.x, sl.y + 8, fillw, 4, 2, wm.theme.accent);
    gfx_fill_circle(s, sl.x + fillw, sl.y + 10, 8, wm.theme.accent);
    gfx_fill_circle(s, sl.x + fillw, sl.y + 10, 4, wm.theme.menu_bg);
    char v[8];
    snprintf(v, sizeof(v), "%u", wm.cfg.volume);
    font_draw(s, F_UI, r.x + r.w - 44, r.y + 46, v, wm.theme.menu_text);
}

static void set_volume_from_mouse(int x) {
    rect_t sl = vol_slider();
    int v = (x - sl.x) * 100 / MAX(1, sl.w);
    v = MAX(0, MIN(100, v));
    if ((uint32_t)v != wm.cfg.volume) {
        wm.cfg.volume = v;
        snd_set_volume(v);
        damage_overlay();
        wm_damage(volume_rect());
    }
}

static void draw_network(surface_t *s) {
    rect_t r = ov_rect;
    panel(s, r);
    char ip[32];
    bool online = net_status(ip, sizeof(ip));
    draw_glyph(s, online ? "net" : "net-off", 32, r.x + 18, r.y + 20, online ? wm.theme.accent : wm.theme.window_text_dim);
    font_draw(s, F_UIB, r.x + 64, r.y + 20, online ? "Ethernet - Connected" : "Not connected", wm.theme.menu_text);
    char line[64];
    if (online) snprintf(line, sizeof(line), "IP address: %s", ip);
    else snprintf(line, sizeof(line), "No network adapter or no DHCP lease");
    font_draw(s, F_UI, r.x + 64, r.y + 20 + F_UIB->height + 2, line, wm.theme.window_text_dim);
}

/* ------------------------------------------------------------------ alt-tab */

static rect_t alttab_rect(void) {
    int n = alttab_count;
    int w = MIN(wm.w - 40, MAX(n, 1) * 100 + 40);
    return mkrect((wm.w - w) / 2, wm.h / 2 - 80, w, 160);
}

void shell_alt_tab(bool forward) {
    if (!alttab_active) {
        alttab_count = 0;
        /* most recently raised windows are at the end of the z-order */
        window_t *list[64];
        int n = 0;
        for (window_t *w = wm.windows; w && n < 64; w = w->next)
            if ((w->visible || w->minimized) && !(w->flags & (WF_POPUP | WF_NO_TASKBAR))) list[n++] = w;
        for (int i = n - 1; i >= 0 && alttab_count < 32; i--) alttab_list[alttab_count++] = list[i];
        if (alttab_count < 1) return;
        alttab_active = true;
        alttab_index = alttab_count > 1 ? (forward ? 1 : alttab_count - 1) : 0;
    } else {
        alttab_index = (alttab_index + (forward ? 1 : alttab_count - 1)) % alttab_count;
    }
    wm_damage(alttab_rect());
}

void shell_alt_release(void) {
    if (!alttab_active) return;
    alttab_active = false;
    wm_damage(alttab_rect());
    window_t *w = alttab_list[alttab_index];
    if (win_find(w->id) == w) win_restore(w);
}

static void draw_alttab(surface_t *s) {
    rect_t r = alttab_rect();
    panel(s, r);
    int x = r.x + 20;
    for (int i = 0; i < alttab_count; i++) {
        window_t *w = alttab_list[i];
        rect_t t = mkrect(x + i * 100, r.y + 20, 96, 96);
        if (i == alttab_index) {
            gfx_fill_rounded(s, t.x, t.y, t.w, t.h, 10, WITH_ALPHA(wm.theme.accent, 80));
            gfx_rounded_rect(s, t.x, t.y, t.w, t.h, 10, wm.theme.accent);
        }
        icon_t *ic = icon_get(w->icon[0] ? w->icon : "file-exec", 48);
        draw_icon(s, ic, t.x + 24, t.y + 24);
    }
    window_t *sel = alttab_list[alttab_index];
    int tw = font_text_width(F_UIB, sel->title);
    font_draw_fit(s, F_UIB, r.x + MAX(12, (r.w - tw) / 2), r.y + 126, sel->title, r.w - 24, wm.theme.menu_text);
}

/* ------------------------------------------------------------------ notifications */

static rect_t toast_rect(int i) {
    rect_t wa = wm_work_area();
    int w = 340, h = 76;
    int y = wm.cfg.taskbar_top ? wa.y + 12 + i * (h + 10) : wa.y + wa.h - 12 - h - i * (h + 10);
    return mkrect(wm.w - w - 12, y, w, h);
}

extern void snd_play_file(const char *path);
__attribute__((weak)) void snd_play_file(const char *path) { UNUSED(path); }

void wm_notify(const char *title, const char *text, const char *icon) {
    /* shift older notifications */
    for (int i = 2; i > 0; i--) toasts[i] = toasts[i - 1];
    toast_t *t = &toasts[0];
    strlcpy(t->title, title, sizeof(t->title));
    strlcpy(t->text, text, sizeof(t->text));
    strlcpy(t->icon, icon ? icon : "about", sizeof(t->icon));
    t->until = uptime_ms() + 5000;
    t->used = true;
    for (int i = 0; i < 3; i++) wm_damage(mkrect(toast_rect(i).x - 30, toast_rect(i).y - 30, 400, 140));
    if (wm.cfg.sounds) snd_play_file("/system/sounds/notify.wav");
    klog("[notify] %s: %s\n", title, text);
}

static void draw_toasts(surface_t *s) {
    for (int i = 0; i < 3; i++) {
        toast_t *t = &toasts[i];
        if (!t->used) continue;
        rect_t r = toast_rect(i);
        panel(s, r);
        icon_t *ic = icon_get(t->icon, 32);
        draw_icon(s, ic, r.x + 16, r.y + (r.h - 32) / 2);
        font_draw_fit(s, F_UIB, r.x + 62, r.y + 16, t->title, r.w - 76, wm.theme.menu_text);
        font_draw_fit(s, F_UI, r.x + 62, r.y + 18 + F_UIB->height, t->text, r.w - 76, wm.theme.window_text_dim);
    }
}

/* ------------------------------------------------------------------ power */

void fs_sync_all(void);
void power_off(void);
void power_reboot(void);
__attribute__((weak)) void fs_sync_all(void) {}

static void do_power(int action) {
    close_overlay();
    power_msg = action == ACT_RESTART ? "Restarting..." : "Shutting down...";
    wm_damage_all();
    /* draw the message immediately */
    surface_t *s = &wm.scr;
    gfx_reset_clip(s);
    for (int y = 0; y < wm.h; y++)
        for (int x = 0; x < wm.w; x++) s->px[y * wm.w + x] = gfx_blend(s->px[y * wm.w + x], 0xFF101218, 200);
    int tw = font_text_width(F_TITLE, power_msg);
    font_draw(s, F_TITLE, (wm.w - tw) / 2, wm.h / 2 - 12, power_msg, 0xFFFFFFFF);
    fb_flush(s, mkrect(0, 0, wm.w, wm.h));
    config_save(&wm.cfg);
    /* ask applications to close, give them a moment */
    for (window_t *w = wm.windows; w; w = w->next) win_close_request(w);
    mutex_unlock(&wm.lock);
    if (wm.cfg.sounds) snd_play_file("/system/sounds/shutdown.wav");
    sleep_ms(1200);
    fs_sync_all();
    if (action == ACT_RESTART) power_reboot();
    else power_off();
    mutex_lock(&wm.lock);
}

/* ------------------------------------------------------------------ public entry points */

void shell_init(void) {
    load_apps();
    shell_desktop_reload();
    update_clock(true);
}

void shell_windows_changed(void) {
    wm_damage(shell_taskbar_rect());
}

void shell_draw(surface_t *s, rect_t clip, int layer) {
    if (layer == 0) { draw_desktop(s, clip); return; }
    if (layer == 1) { draw_taskbar(s, clip); return; }
    if (overlay != OV_NONE) {
        rect_t big = mkrect(ov_rect.x - 30, ov_rect.y - 30, ov_rect.w + 60, ov_rect.h + 66);
        if (rect_intersect(big, clip, 0)) {
            switch (overlay) {
            case OV_START: draw_start(s); break;
            case OV_MENU: draw_menu(s); break;
            case OV_CALENDAR: draw_calendar(s); break;
            case OV_VOLUME: draw_volume(s); break;
            case OV_NETWORK: draw_network(s); break;
            }
        }
    }
    if (alttab_active) draw_alttab(s);
    draw_toasts(s);
}

static void toggle_start(void) {
    if (overlay == OV_START) close_overlay();
    else { close_overlay(); open_start(); }
}

bool shell_mouse(int type, int x, int y, uint32_t button, int clicks, int wheel) {
    rect_t tb = shell_taskbar_rect();
    if (type == EV_MOUSE_MOVE) {
        int nh = -1;
        if (rect_contains(tb, x, y)) {
            layout_taskbar();
            if (rect_contains(start_btn_rect(), x, y)) nh = -2;
            else if (rect_contains(clock_rect(), x, y)) nh = -3;
            else if (rect_contains(volume_rect(), x, y)) nh = -4;
            else if (rect_contains(network_rect(), x, y)) nh = -5;
            else if (rect_contains(showdesk_rect(), x, y)) nh = -6;
            else for (int i = 0; i < tb_nbuttons; i++) if (rect_contains(tb_buttons[i].r, x, y)) nh = i;
        }
        if (nh != tb_hover) { tb_hover = nh; wm_damage(tb); }
        if (overlay == OV_START) {
            int idx[MAX_APPS];
            int n = start_matches(idx, MAX_APPS);
            int h = -1;
            for (int k = 0; k < n; k++) if (rect_contains(tile_rect(ov_rect, k), x, y)) h = k;
            bool ph = rect_contains(power_btn_rect(ov_rect), x, y);
            if (h != start_hover || ph != power_hover) { start_hover = h; power_hover = ph; damage_overlay(); }
        } else if (overlay == OV_MENU) {
            int h = menu_item_at(x, y);
            if (h != menu_hover) { menu_hover = h; damage_overlay(); }
        } else if (overlay == OV_VOLUME && vol_dragging) {
            set_volume_from_mouse(x);
        }
        return overlay != OV_NONE && rect_contains(ov_rect, x, y);
    }
    if (type == EV_MOUSE_WHEEL) {
        if (overlay == OV_START && rect_contains(ov_rect, x, y)) {
            int rows = (napps + SM_COLS - 1) / SM_COLS;
            int vis = (ov_rect.h - 64 - 26 - 62) / TILE_H;
            int maxs = MAX(0, (rows - vis) * TILE_H);
            start_scroll = MAX(0, MIN(maxs, start_scroll + wheel * 40));
            damage_overlay();
            return true;
        }
        if (overlay == OV_VOLUME) {
            int v = (int)wm.cfg.volume - wheel * 5;
            wm.cfg.volume = MAX(0, MIN(100, v));
            snd_set_volume((int)wm.cfg.volume);
            damage_overlay();
            wm_damage(volume_rect());
            return true;
        }
        if (rect_contains(volume_rect(), x, y)) {
            int v = (int)wm.cfg.volume - wheel * 5;
            wm.cfg.volume = MAX(0, MIN(100, v));
            snd_set_volume((int)wm.cfg.volume);
            wm_damage(volume_rect());
            return true;
        }
        return overlay != OV_NONE;
    }
    if (type == EV_MOUSE_UP) {
        if (overlay == OV_VOLUME && vol_dragging) {
            vol_dragging = false;
            config_save(&wm.cfg);
            return true;
        }
        if (overlay == OV_MENU) {
            int h = menu_item_at(x, y);
            if (h >= 0) {
                int a = menu_items[h].action;
                close_overlay();
                run_action(a);
            }
            return true;
        }
        return false;
    }
    /* EV_MOUSE_DOWN */
    /* clicks inside an open overlay */
    if (overlay != OV_NONE && rect_contains(ov_rect, x, y)) {
        switch (overlay) {
        case OV_START: {
            if (rect_contains(power_btn_rect(ov_rect), x, y)) {
                menu_count = 0;
                add_menu("Restart", "restart", ACT_RESTART);
                add_menu("Shut down", "power", ACT_SHUTDOWN);
                rect_t pb = power_btn_rect(ov_rect);
                close_overlay();
                open_menu(pb.x - 180, pb.y - 8 - 76);
                power_menu_from_start = true;
                return true;
            }
            int idx[MAX_APPS];
            int n = start_matches(idx, MAX_APPS);
            for (int k = 0; k < n; k++) {
                if (rect_contains(tile_rect(ov_rect, k), x, y)) {
                    app_t *a = &apps[idx[k]];
                    close_overlay();
                    launch_app(a);
                    return true;
                }
            }
            return true;
        }
        case OV_CALENDAR:
            if (rect_contains(cal_nav_rect(-1), x, y)) {
                if (--cal_month < 1) { cal_month = 12; cal_year--; }
                damage_overlay();
            } else if (rect_contains(cal_nav_rect(1), x, y)) {
                if (++cal_month > 12) { cal_month = 1; cal_year++; }
                damage_overlay();
            }
            return true;
        case OV_VOLUME:
            if (rect_contains(mkrect(vol_slider().x - 10, vol_slider().y - 6, vol_slider().w + 20, 32), x, y)) {
                vol_dragging = true;
                set_volume_from_mouse(x);
            }
            return true;
        case OV_MENU:
        case OV_NETWORK:
            return true;
        }
    }
    int was = overlay;
    if (overlay != OV_NONE) close_overlay();
    /* taskbar */
    if (rect_contains(tb, x, y)) {
        if (button != MOUSE_LEFT) return true;
        layout_taskbar();
        if (rect_contains(start_btn_rect(), x, y)) { if (was != OV_START) open_start(); return true; }
        if (rect_contains(clock_rect(), x, y)) {
            if (was != OV_CALENDAR) {
                int d;
                today(&cal_year, &cal_month, &d);
                open_overlay(OV_CALENDAR, popup_rect_at(clock_rect().x + clock_rect().w - 300, 300, 356));
            }
            return true;
        }
        if (rect_contains(volume_rect(), x, y)) {
            if (was != OV_VOLUME) open_overlay(OV_VOLUME, popup_rect_at(volume_rect().x - 120, 300, 84));
            return true;
        }
        if (rect_contains(network_rect(), x, y)) {
            if (was != OV_NETWORK) open_overlay(OV_NETWORK, popup_rect_at(network_rect().x - 150, 340, 80));
            return true;
        }
        if (rect_contains(showdesk_rect(), x, y)) {
            for (window_t *w = wm.windows; w; w = w->next)
                if (w->visible && !(w->flags & (WF_NO_TASKBAR | WF_POPUP))) win_minimize(w);
            return true;
        }
        for (int i = 0; i < tb_nbuttons; i++) {
            if (!rect_contains(tb_buttons[i].r, x, y)) continue;
            window_t *w = win_find(tb_buttons[i].id);
            if (!w) break;
            if (w->minimized || wm.focused != w) win_restore(w);
            else win_minimize(w);
            return true;
        }
        return true;
    }
    if (was != OV_NONE) return true;
    /* desktop */
    int hit = dicon_at(x, y);
    if (hit != dicon_sel) {
        if (dicon_sel >= 0) wm_damage(dicons[dicon_sel].r);
        dicon_sel = hit;
        if (hit >= 0) wm_damage(dicons[hit].r);
    }
    if (button == MOUSE_LEFT && hit >= 0 && clicks >= 2) {
        launch_app(dicons[hit].app);
        return true;
    }
    if (button == MOUSE_RIGHT) {
        menu_count = 0;
        if (hit >= 0) {
            static char open_label[64];
            snprintf(open_label, sizeof(open_label), "Open %s", dicons[hit].app->name);
            add_menu(open_label, "open", 100 + hit);
            add_menu(0, 0, 0);
        }
        add_menu("Open Terminal", "menu", ACT_TERMINAL);
        add_menu("Open Files", "folder", ACT_FILES);
        add_menu(0, 0, 0);
        add_menu("Change wallpaper...", "image", ACT_WALLPAPER);
        add_menu("Personalize...", "brush", ACT_PERSONALIZE);
        add_menu("Display settings...", "computer", ACT_DISPLAY);
        add_menu(0, 0, 0);
        add_menu("Task Manager", "list", ACT_TASKMGR);
        open_menu(x, y);
        return true;
    }
    return true;
}

bool shell_key(uint32_t key, uint32_t ch, uint32_t mods, bool pressed) {
    if (key == 0xFFFF) { toggle_start(); return true; }
    if (!pressed) return overlay == OV_START;
    if (overlay == OV_NONE) return false;
    if (key == KEY_ESC) { close_overlay(); return true; }
    if (overlay == OV_MENU) {
        if (key == KEY_DOWN || key == KEY_UP) {
            int dir = key == KEY_DOWN ? 1 : -1;
            int h = menu_hover;
            for (int i = 0; i < menu_count; i++) {
                h = (h + dir + menu_count) % menu_count;
                if (menu_items[h].label) break;
            }
            menu_hover = h;
            damage_overlay();
        } else if (key == KEY_ENTER && menu_hover >= 0) {
            int a = menu_items[menu_hover].action;
            close_overlay();
            run_action(a);
        }
        return true;
    }
    if (overlay != OV_START) return false;
    int idx[MAX_APPS];
    int n = start_matches(idx, MAX_APPS);
    if (key == KEY_ENTER) {
        int k = start_sel >= 0 ? start_sel : 0;
        if (k < n) {
            app_t *a = &apps[idx[k]];
            close_overlay();
            launch_app(a);
        }
        return true;
    }
    if (key == KEY_RIGHT || key == KEY_LEFT || key == KEY_DOWN || key == KEY_UP) {
        int d = key == KEY_RIGHT ? 1 : key == KEY_LEFT ? -1 : key == KEY_DOWN ? SM_COLS : -SM_COLS;
        int k = start_sel < 0 ? 0 : start_sel + d;
        if (n) start_sel = MAX(0, MIN(n - 1, k));
        damage_overlay();
        return true;
    }
    if (key == KEY_BACKSPACE) {
        size_t l = strlen(search);
        if (l) {
            /* remove the last UTF-8 character */
            int i = utf8_prev(search, (int)l);
            search[i] = 0;
        }
        start_sel = -1;
        start_scroll = 0;
        damage_overlay();
        return true;
    }
    if (ch >= ' ' && !(mods & (MOD_CTRL | MOD_ALT))) {
        char enc[4];
        int len = utf8_encode(ch, enc);
        size_t l = strlen(search);
        if (l + len < sizeof(search)) {
            memcpy(search + l, enc, len);
            search[l + len] = 0;
        }
        start_sel = 0;
        start_scroll = 0;
        damage_overlay();
        return true;
    }
    return true;
}

void shell_tick(void) {
    update_clock(false);
    uint64_t now = uptime_ms();
    for (int i = 0; i < 3; i++) {
        if (toasts[i].used && now > toasts[i].until) {
            wm_damage(mkrect(toast_rect(i).x - 30, toast_rect(i).y - 30, 400, 140));
            toasts[i].used = false;
        }
    }
    /* compact the toast list */
    for (int i = 0; i < 2; i++) {
        if (!toasts[i].used && toasts[i + 1].used) {
            toasts[i] = toasts[i + 1];
            toasts[i + 1].used = false;
            wm_damage(mkrect(toast_rect(i).x - 30, toast_rect(i + 1).y - 30, 400, 260));
        }
    }
    static bool last_online;
    char ip[32];
    bool online = net_status(ip, sizeof(ip));
    if (online != last_online) {
        last_online = online;
        wm_damage(network_rect());
        if (online) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Connected, IP address %s", ip);
            wm_notify("Network", msg, "network");
        }
    }
    /* menu actions for desktop icons are encoded as 100 + index */
}
