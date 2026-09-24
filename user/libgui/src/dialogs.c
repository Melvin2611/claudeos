/* libgui modal dialogs: message box, text input, file chooser, colour picker */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

void ui_set_modal(ui_window_t *w);
ui_window_t *ui_get_modal(void);

static int dlg_result;
static bool dlg_done;

static void run_modal(ui_window_t *win) {
    ui_window_t *prev = ui_get_modal();
    ui_set_modal(win);
    dlg_done = false;
    while (!dlg_done && win->alive) {
        if (!ui_step(-1)) break;
    }
    ui_set_modal(prev);
}

static ui_window_t *dialog_window(ui_window_t *parent, const char *title, int w, int h, const char *icon) {
    gui_wincreate_t req;
    memset(&req, 0, sizeof(req));
    req.x = -1;
    req.y = -1;
    req.w = w;
    req.h = h;
    req.flags = WF_DIALOG | WF_CENTER;
    req.parent = parent ? parent->id : 0;
    strlcpy(req.title, title, sizeof(req.title));
    strlcpy(req.icon, icon ? icon : "about", sizeof(req.icon));
    return ui_window_ex(&req);
}

static bool dlg_close(ui_window_t *w) {
    (void)w;
    dlg_result = -1;
    dlg_done = true;
    return true;
}

/* ------------------------------------------------------------------ message box */
static void msg_btn(ui_widget_t *b) {
    dlg_result = b->id;
    dlg_done = true;
}

static void msg_key(ui_window_t *w, gui_event_t *ev) {
    if (ev->key == KEY_ESC) { dlg_result = -1; dlg_done = true; }
}

static void msg_icon(ui_widget_t *w, surface_t *s) { ui_draw_icon(s, "about", 32, w->r.x, w->r.y); }

static int wrap_lines(const char *text, int width) {
    int lines = 0;
    const char *p = text;
    while (*p) {
        int n = 0, last_space = -1;
        while (p[n] && p[n] != '\n') {
            if (p[n] == ' ') last_space = n;
            int nn = utf8_next(p, n);
            if (font_text_width_n(&ui_font, p, nn) > width && n > 0) { if (last_space > 0) n = last_space; break; }
            n = nn;
        }
        lines++;
        p += n;
        if (*p == ' ' || *p == '\n') p++;
    }
    return lines ? lines : 1;
}

int ui_msgbox(ui_window_t *parent, const char *title, const char *text, const char *buttons) {
    ui_init();
    int w = 400, textw = w - 90;
    int lines = wrap_lines(text, textw);
    int h = MAX(120, 36 + lines * (ui_font.height + 2) + 70);
    const char *icon = strstr(title, "rror") ? "about" : "about";
    ui_window_t *win = dialog_window(parent, title, w, h, icon);
    if (!win) return -1;
    win->on_close = dlg_close;
    win->on_key = msg_key;
    ui_widget_t *ic = ui_canvas(win, 22, 26, 32, 32, msg_icon, 0);
    ic->focusable = false;
    ui_label(win, 70, 28, textw, lines * (ui_font.height + 2) + 4, text);
    /* buttons, right aligned */
    char buf[128];
    strlcpy(buf, buttons ? buttons : "OK", sizeof(buf));
    char *names[4];
    int n = 0;
    for (char *tok = strtok(buf, "|"); tok && n < 4; tok = strtok(0, "|")) names[n++] = tok;
    int x = w - 16;
    ui_widget_t *first = 0;
    for (int i = n - 1; i >= 0; i--) {
        int bw = MAX(88, font_text_width(&ui_font, names[i]) + 28);
        x -= bw;
        ui_widget_t *b = ui_button(win, x, h - 48, bw, 32, names[i], msg_btn);
        b->id = i;
        if (i == 0) { ui_button_style(b, BTN_PRIMARY); first = b; }
        x -= 8;
    }
    ui_panel(win, 0, h - 64, w, 1, ui_theme.input_border);
    ui_focus(win, first);
    dlg_result = -1;
    run_modal(win);
    if (win->alive) ui_window_close(win);
    return dlg_result;
}

/* ------------------------------------------------------------------ text input */
static ui_widget_t *input_box;

static void input_ok(ui_widget_t *w) { (void)w; dlg_result = 0; dlg_done = true; }
static void input_cancel(ui_widget_t *w) { (void)w; dlg_result = -1; dlg_done = true; }

char *ui_input(ui_window_t *parent, const char *title, const char *prompt, const char *initial) {
    ui_init();
    int w = 420, h = 168;
    ui_window_t *win = dialog_window(parent, title, w, h, "editor");
    if (!win) return 0;
    win->on_close = dlg_close;
    win->on_key = msg_key;
    ui_label(win, 20, 20, w - 40, 20, prompt);
    input_box = ui_textbox(win, 20, 48, w - 40, 32, initial ? initial : "");
    input_box->on_activate = input_ok;
    ui_textbox_select_all(input_box);
    ui_widget_t *ok = ui_button(win, w - 20 - 88 - 8 - 88, h - 48, 88, 32, "OK", input_ok);
    ui_button_style(ok, BTN_PRIMARY);
    ui_button(win, w - 20 - 88, h - 48, 88, 32, "Cancel", input_cancel);
    ui_focus(win, input_box);
    dlg_result = -1;
    run_modal(win);
    char *res = 0;
    if (dlg_result == 0 && win->alive) res = strdup(ui_get_text(input_box));
    if (win->alive) ui_window_close(win);
    return res;
}

/* ------------------------------------------------------------------ file chooser */
typedef struct {
    ui_window_t *win;
    ui_widget_t *path, *list, *name, *ok;
    char dir[256];
    bool save;
    char filter[64];
} filedlg_t;
static filedlg_t fd;

typedef struct { char name[256]; bool dir; long size; } fentry_t;
static fentry_t *fentries;
static int nfentries;

static int fcmp(const void *a, const void *b) {
    const fentry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static bool filter_ok(const char *name) {
    if (!fd.filter[0]) return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char f[64];
    strlcpy(f, fd.filter, sizeof(f));
    for (char *t = strtok(f, ";"); t; t = strtok(0, ";"))
        if (!strcasecmp(dot + 1, t)) return true;
    return false;
}

static void fd_load(const char *dir) {
    char norm[256];
    if (dir != fd.dir) strlcpy(norm, dir, sizeof(norm));
    else strlcpy(norm, fd.dir, sizeof(norm));
    DIR *d = opendir(norm);
    if (!d) return;
    strlcpy(fd.dir, norm, sizeof(fd.dir));
    ui_set_text(fd.path, fd.dir);
    free(fentries);
    fentries = 0;
    nfentries = 0;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        bool isdir = e->d_type == DT_DIR;
        if (!isdir && !filter_ok(e->d_name)) continue;
        if (nfentries == cap) { cap = cap ? cap * 2 : 32; fentries = realloc(fentries, cap * sizeof(fentry_t)); }
        strlcpy(fentries[nfentries].name, e->d_name, 256);
        fentries[nfentries].dir = isdir;
        fentries[nfentries].size = e->d_size;
        nfentries++;
    }
    closedir(d);
    qsort(fentries, nfentries, sizeof(fentry_t), fcmp);
    ui_list_clear(fd.list);
    for (int i = 0; i < nfentries; i++) {
        char row[320], sz[32];
        if (fentries[i].dir) strcpy(sz, "Folder");
        else if (fentries[i].size < 1024) snprintf(sz, sizeof(sz), "%ld B", fentries[i].size);
        else if (fentries[i].size < 1024 * 1024) snprintf(sz, sizeof(sz), "%ld KB", fentries[i].size / 1024);
        else snprintf(sz, sizeof(sz), "%.1f MB", fentries[i].size / 1048576.0);
        snprintf(row, sizeof(row), "%s\t%s", fentries[i].name, sz);
        const char *icon = "file";
        const char *dot = strrchr(fentries[i].name, '.');
        if (fentries[i].dir) icon = "folder";
        else if (dot && (!strcasecmp(dot, ".txt") || !strcasecmp(dot, ".md") || !strcasecmp(dot, ".cfg") || !strcasecmp(dot, ".c") || !strcasecmp(dot, ".sh"))) icon = "file-text";
        else if (dot && !strcasecmp(dot, ".bmp")) icon = "file-image";
        else if (dot && !strcasecmp(dot, ".wav")) icon = "file-audio";
        ui_list_add(fd.list, row, icon, 0);
    }
}

static void join(char *out, size_t n, const char *dir, const char *name) {
    if (name[0] == '/') { strlcpy(out, name, n); return; }
    if (!strcmp(dir, "/")) snprintf(out, n, "/%s", name);
    else snprintf(out, n, "%s/%s", dir, name);
}

static void fd_up(ui_widget_t *w) {
    (void)w;
    char p[256];
    strlcpy(p, fd.dir, sizeof(p));
    char *s = strrchr(p, '/');
    if (s && s != p) *s = 0;
    else strcpy(p, "/");
    fd_load(p);
}

static void fd_home(ui_widget_t *w) { (void)w; fd_load("/home"); }
static void fd_root(ui_widget_t *w) { (void)w; fd_load("/"); }

static void fd_select(ui_widget_t *w) {
    int i = w->ival;
    if (i >= 0 && i < nfentries && !fentries[i].dir) ui_set_text(fd.name, fentries[i].name);
}

static void fd_accept(ui_widget_t *w) {
    (void)w;
    const char *name = ui_get_text(fd.name);
    int i = fd.list->ival;
    if (!name[0] && i >= 0 && i < nfentries && fentries[i].dir) {
        char p[256];
        join(p, sizeof(p), fd.dir, fentries[i].name);
        fd_load(p);
        return;
    }
    if (!name[0]) return;
    char full[256];
    join(full, sizeof(full), fd.dir, name);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        fd_load(full);
        ui_set_text(fd.name, "");
        return;
    }
    if (fd.save && stat(full, &st) == 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "\"%s\" already exists. Do you want to replace it?", name);
        if (ui_msgbox(fd.win, "Confirm Save", msg, "Replace|Cancel") != 0) return;
    }
    if (!fd.save && stat(full, &st) != 0) {
        ui_msgbox(fd.win, "Open", "The file does not exist.", "OK");
        return;
    }
    dlg_result = 0;
    dlg_done = true;
}

static void fd_activate(ui_widget_t *w) {
    int i = w->ival;
    if (i < 0 || i >= nfentries) return;
    if (fentries[i].dir) {
        char p[256];
        join(p, sizeof(p), fd.dir, fentries[i].name);
        fd_load(p);
    } else {
        ui_set_text(fd.name, fentries[i].name);
        fd_accept(w);
    }
}

static void fd_path_enter(ui_widget_t *w) { fd_load(ui_get_text(w)); }
static void fd_cancel(ui_widget_t *w) { (void)w; dlg_result = -1; dlg_done = true; }

char *ui_file_dialog(ui_window_t *parent, bool save, const char *title, const char *dir, const char *name,
                     const char *filter) {
    ui_init();
    int w = 580, h = 440;
    memset(&fd, 0, sizeof(fd));
    fd.save = save;
    if (filter) strlcpy(fd.filter, filter, sizeof(fd.filter));
    ui_window_t *win = dialog_window(parent, title ? title : (save ? "Save As" : "Open"), w, h, "folder");
    if (!win) return 0;
    fd.win = win;
    win->on_close = dlg_close;
    win->on_key = msg_key;
    ui_toolbutton(win, 12, 12, 32, "up", "Up", fd_up);
    ui_toolbutton(win, 48, 12, 32, "home", "Home", fd_home);
    ui_toolbutton(win, 84, 12, 32, "computer", "Root", fd_root);
    fd.path = ui_textbox(win, 124, 12, w - 136, 32, "");
    fd.path->on_activate = fd_path_enter;
    fd.list = ui_list(win, 12, 54, w - 24, h - 54 - 100);
    static const ui_column_t cols[] = { { "Name", 400 }, { "Size", -1 } };
    ui_list_set_columns(fd.list, cols, 2);
    fd.list->on_change = fd_select;
    fd.list->on_activate = fd_activate;
    ui_label(win, 12, h - 86, 90, 30, "File name:");
    fd.name = ui_textbox(win, 100, h - 88, w - 112, 32, name ? name : "");
    fd.name->on_activate = fd_accept;
    fd.ok = ui_button(win, w - 12 - 96 - 8 - 96, h - 46, 96, 32, save ? "Save" : "Open", fd_accept);
    ui_button_style(fd.ok, BTN_PRIMARY);
    ui_button(win, w - 12 - 96, h - 46, 96, 32, "Cancel", fd_cancel);
    fd_load(dir && *dir ? dir : "/home");
    ui_focus(win, save ? fd.name : fd.list);
    dlg_result = -1;
    run_modal(win);
    char *res = 0;
    if (dlg_result == 0 && win->alive) {
        char full[256];
        join(full, sizeof(full), fd.dir, ui_get_text(fd.name));
        res = strdup(full);
    }
    if (win->alive) ui_window_close(win);
    return res;
}

/* ------------------------------------------------------------------ colour picker */
static ui_widget_t *cr, *cg, *cb, *cprev, *chex;
static uint32_t cur_color;

static void color_update_widgets(void) {
    cr->ival = COL_R(cur_color);
    cg->ival = COL_G(cur_color);
    cb->ival = COL_B(cur_color);
    cprev->color = cur_color;
    char hex[16];
    snprintf(hex, sizeof(hex), "#%06X", cur_color & 0xFFFFFF);
    ui_set_text(chex, hex);
    ui_widget_invalidate(cr);
    ui_widget_invalidate(cg);
    ui_widget_invalidate(cb);
    ui_widget_invalidate(cprev);
}

static void color_slider(ui_widget_t *w) {
    (void)w;
    cur_color = RGB(cr->ival, cg->ival, cb->ival);
    cprev->color = cur_color;
    char hex[16];
    snprintf(hex, sizeof(hex), "#%06X", cur_color & 0xFFFFFF);
    ui_set_text(chex, hex);
    ui_widget_invalidate(cprev);
}

static void color_swatch(ui_widget_t *w) {
    cur_color = w->color;
    color_update_widgets();
}

static void color_hex(ui_widget_t *w) {
    const char *t = ui_get_text(w);
    if (*t == '#') t++;
    cur_color = 0xFF000000u | (uint32_t)strtoul(t, 0, 16);
    color_update_widgets();
}

bool ui_color_dialog(ui_window_t *parent, uint32_t *color) {
    ui_init();
    static const uint32_t palette[] = {
        0x000000, 0x404040, 0x808080, 0xC0C0C0, 0xFFFFFF, 0x7F1D1D, 0xDC2626, 0xF87171,
        0x9A3412, 0xEA580C, 0xFB923C, 0xD97757, 0xA16207, 0xEAB308, 0xFDE047, 0x3F6212,
        0x65A30D, 0xA3E635, 0x065F46, 0x10B981, 0x6EE7B7, 0x155E75, 0x06B6D4, 0x67E8F9,
        0x1E3A8A, 0x2563EB, 0x60A5FA, 0x4C1D95, 0x7C3AED, 0xA78BFA, 0x831843, 0xDB2777,
    };
    int w = 420, h = 330;
    ui_window_t *win = dialog_window(parent, "Choose Color", w, h, "paint");
    if (!win) return false;
    win->on_close = dlg_close;
    win->on_key = msg_key;
    cur_color = *color | 0xFF000000u;
    for (int i = 0; i < 32; i++)
        ui_swatch(win, 16 + (i % 8) * 36, 16 + (i / 8) * 36, 30, 30, 0xFF000000u | palette[i], color_swatch);
    cprev = ui_swatch(win, w - 104, 16, 88, 102, cur_color, 0);
    ui_label(win, 16, 170, 20, 24, "R");
    cr = ui_slider(win, 36, 170, w - 60, 24, 0, 255, 0);
    ui_label(win, 16, 200, 20, 24, "G");
    cg = ui_slider(win, 36, 200, w - 60, 24, 0, 255, 0);
    ui_label(win, 16, 230, 20, 24, "B");
    cb = ui_slider(win, 36, 230, w - 60, 24, 0, 255, 0);
    cr->on_change = cg->on_change = cb->on_change = color_slider;
    chex = ui_textbox(win, 16, h - 48, 110, 32, "");
    chex->on_activate = color_hex;
    ui_widget_t *ok = ui_button(win, w - 16 - 88 - 8 - 88, h - 48, 88, 32, "OK", input_ok);
    ui_button_style(ok, BTN_PRIMARY);
    ui_button(win, w - 16 - 88, h - 48, 88, 32, "Cancel", input_cancel);
    color_update_widgets();
    dlg_result = -1;
    run_modal(win);
    bool ok_pressed = dlg_result == 0;
    if (ok_pressed) *color = cur_color;
    if (win->alive) ui_window_close(win);
    return ok_pressed;
}
