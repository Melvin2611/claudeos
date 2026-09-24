/* Files - the ClaudeOS file manager */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <claudeos.h>

#define SIDEBAR_W 170
#define TOOLBAR_H 48
#define STATUS_H 26

typedef struct {
    char name[256];
    bool dir;
    long size;
    time_t mtime;
} entry_t;

static ui_window_t *win;
static ui_widget_t *list, *path_box, *places, *status, *btn_back, *btn_fwd, *btn_up, *btn_view;
static char cwd[256] = "/home";
static entry_t *entries;
static int nentries;
static bool grid_view = true;
static char history[32][256];
static int hist_pos = -1, hist_len;
static char clip_path[256];
static bool clip_cut;

typedef struct { const char *name; const char *path; const char *icon; } place_t;
static const place_t place_list[] = {
    { "Home", "/home", "home" },
    { "Root", "/", "drive" },
    { "Programs", "/bin", "file-exec" },
    { "System", "/system", "settings" },
    { "Temporary", "/tmp", "folder" },
    { "Devices", "/dev", "drive" },
};

static void load_dir(const char *path, bool push);

/* ------------------------------------------------------------------ helpers */
static void join(char *out, size_t n, const char *dir, const char *name) {
    if (!strcmp(dir, "/")) snprintf(out, n, "/%s", name);
    else snprintf(out, n, "%s/%s", dir, name);
}

static const char *icon_for(entry_t *e) {
    if (e->dir) return "folder";
    const char *dot = strrchr(e->name, '.');
    if (!dot) return !strcmp(cwd, "/bin") ? "file-exec" : "file";
    dot++;
    if (!strcasecmp(dot, "txt") || !strcasecmp(dot, "md") || !strcasecmp(dot, "cfg") || !strcasecmp(dot, "conf") ||
        !strcasecmp(dot, "c") || !strcasecmp(dot, "h") || !strcasecmp(dot, "sh") || !strcasecmp(dot, "log"))
        return "file-text";
    if (!strcasecmp(dot, "bmp") || !strcasecmp(dot, "icn")) return "file-image";
    if (!strcasecmp(dot, "wav")) return "file-audio";
    return "file";
}

static const char *type_for(entry_t *e) {
    if (e->dir) return "Folder";
    const char *ic = icon_for(e);
    if (!strcmp(ic, "file-text")) return "Text document";
    if (!strcmp(ic, "file-image")) return "Image";
    if (!strcmp(ic, "file-audio")) return "Audio";
    if (!strcmp(ic, "file-exec")) return "Program";
    return "File";
}

static int cmp(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static entry_t *selected(void) {
    int i = list->ival;
    return i >= 0 && i < nentries ? &entries[i] : 0;
}

static void update_status(void) {
    char buf[200], free_s[32] = "";
    kstatfs_t st;
    if (statfs(cwd, &st) == 0 && st.total_bytes) {
        char f[16];
        format_size(st.free_bytes, f, sizeof(f));
        snprintf(free_s, sizeof(free_s), "  \xE2\x80\xA2  %s free (%s)", f, st.fstype);
    }
    entry_t *e = selected();
    if (e && !e->dir) {
        char sz[16];
        format_size(e->size, sz, sizeof(sz));
        snprintf(buf, sizeof(buf), "%d items  \xE2\x80\xA2  \"%s\" selected (%s)%s", nentries, e->name, sz, free_s);
    } else if (e) {
        snprintf(buf, sizeof(buf), "%d items  \xE2\x80\xA2  \"%s\" selected%s", nentries, e->name, free_s);
    } else {
        snprintf(buf, sizeof(buf), "%d items%s", nentries, free_s);
    }
    ui_set_text(status, buf);
}

static void fill_list(void) {
    ui_list_clear(list);
    ui_list_set_icon_mode(list, grid_view);
    ui_list_set_row_height(list, grid_view ? 26 : 28);
    static const ui_column_t cols[] = { { "Name", 280 }, { "Size", 100 }, { "Type", 130 }, { "Modified", -1 } };
    ui_list_set_columns(list, cols, grid_view ? 0 : 4);
    for (int i = 0; i < nentries; i++) {
        entry_t *e = &entries[i];
        if (grid_view) {
            ui_list_add(list, e->name, icon_for(e), 0);
        } else {
            char row[512], sz[24], tb[32];
            if (e->dir) strcpy(sz, "");
            else format_size(e->size, sz, sizeof(sz));
            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", localtime(&e->mtime));
            snprintf(row, sizeof(row), "%s\t%s\t%s\t%s", e->name, sz, type_for(e), tb);
            ui_list_add(list, row, icon_for(e), 0);
        }
    }
    update_status();
}

static void update_buttons(void) {
    ui_enable(btn_back, hist_pos > 0);
    ui_enable(btn_fwd, hist_pos < hist_len - 1);
    ui_enable(btn_up, strcmp(cwd, "/") != 0);
    for (int i = 0; i < (int)(sizeof(place_list) / sizeof(place_list[0])); i++)
        if (!strcmp(place_list[i].path, cwd)) { places->ival = i; ui_widget_invalidate(places); return; }
    places->ival = -1;
    ui_widget_invalidate(places);
}

static void load_dir(const char *path, bool push) {
    DIR *d = opendir(path);
    if (!d) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Cannot open \"%s\": %s", path, strerror(errno));
        ui_msgbox(win, "Files", msg, "OK");
        ui_set_text(path_box, cwd);
        return;
    }
    char resolved[256];
    strlcpy(resolved, path, sizeof(resolved));
    /* normalise via chdir/getcwd */
    if (chdir(path) == 0) getcwd(resolved, sizeof(resolved));
    strlcpy(cwd, resolved, sizeof(cwd));
    free(entries);
    entries = 0;
    nentries = 0;
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (nentries == cap) { cap = cap ? cap * 2 : 64; entries = realloc(entries, cap * sizeof(entry_t)); }
        entry_t *e = &entries[nentries++];
        strlcpy(e->name, de->d_name, sizeof(e->name));
        e->dir = de->d_type == DT_DIR;
        e->size = de->d_size;
        e->mtime = de->d_mtime;
    }
    closedir(d);
    qsort(entries, nentries, sizeof(entry_t), cmp);
    if (push) {
        if (hist_pos < 31) hist_pos++;
        else memmove(history, history + 1, sizeof(history[0]) * 31);
        strlcpy(history[hist_pos], cwd, sizeof(history[0]));
        hist_len = hist_pos + 1;
    }
    ui_set_text(path_box, cwd);
    char title[300];
    snprintf(title, sizeof(title), "%s - Files", !strcmp(cwd, "/") ? "/" : ui_basename(cwd));
    ui_set_title(win, title);
    fill_list();
    update_buttons();
}

static void refresh(void) {
    int sel = list->ival;
    load_dir(cwd, false);
    if (sel >= 0) ui_list_select(list, MIN(sel, nentries - 1));
}

static void select_name(const char *name) {
    for (int i = 0; i < nentries; i++)
        if (!strcmp(entries[i].name, name)) { ui_list_select(list, i); update_status(); return; }
}

/* ------------------------------------------------------------------ file operations */
static int delete_tree(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        for (;;) {
            DIR *d = opendir(path);
            if (!d) return -1;
            struct dirent *e = readdir(d);
            char child[512] = "";
            if (e) join(child, sizeof(child), path, e->d_name);
            closedir(d);
            if (!e) break;
            if (delete_tree(child) < 0) return -1;
        }
        return rmdir(path);
    }
    return unlink(path);
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0755) < 0 && errno != EEXIST) return -1;
        DIR *d = opendir(src);
        struct dirent *e;
        int r = 0;
        while (d && (e = readdir(d))) {
            char s[512], t[512];
            join(s, sizeof(s), src, e->d_name);
            join(t, sizeof(t), dst, e->d_name);
            if (copy_tree(s, t) < 0) r = -1;
        }
        if (d) closedir(d);
        return r;
    }
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { close(in); return -1; }
    char buf[8192];
    ssize_t n;
    int r = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, n) != n) { r = -1; break; }
    close(in);
    close(out);
    return r;
}

static void open_entry(entry_t *e, bool with_editor) {
    char full[512];
    join(full, sizeof(full), cwd, e->name);
    if (e->dir) { load_dir(full, true); return; }
    if (with_editor) gui_launch("/bin/editor", full);
    else gui_launch(full, 0);
}

static void do_delete(void) {
    entry_t *e = selected();
    if (!e) return;
    char msg[400];
    snprintf(msg, sizeof(msg), "Do you really want to delete %s \"%s\"%s?", e->dir ? "the folder" : "the file",
             e->name, e->dir ? " and everything inside it" : "");
    if (ui_msgbox(win, "Delete", msg, "Delete|Cancel") != 0) return;
    char full[512];
    join(full, sizeof(full), cwd, e->name);
    if (delete_tree(full) < 0) {
        snprintf(msg, sizeof(msg), "Could not delete \"%s\": %s", e->name, strerror(errno));
        ui_msgbox(win, "Delete", msg, "OK");
    }
    refresh();
}

static void do_rename(void) {
    entry_t *e = selected();
    if (!e) return;
    char *name = ui_input(win, "Rename", "New name:", e->name);
    if (!name) return;
    if (*name && strcmp(name, e->name)) {
        char from[512], to[512];
        join(from, sizeof(from), cwd, e->name);
        join(to, sizeof(to), cwd, name);
        if (rename(from, to) < 0) {
            char msg[400];
            snprintf(msg, sizeof(msg), "Could not rename: %s", strerror(errno));
            ui_msgbox(win, "Rename", msg, "OK");
        } else {
            refresh();
            select_name(name);
        }
    }
    free(name);
}

static void do_new(bool folder) {
    char *name = ui_input(win, folder ? "New Folder" : "New File", folder ? "Folder name:" : "File name:",
                          folder ? "New Folder" : "New Document.txt");
    if (!name) return;
    if (*name) {
        char full[512];
        join(full, sizeof(full), cwd, name);
        int r;
        if (folder) r = mkdir(full, 0755);
        else {
            r = open(full, O_WRONLY | O_CREAT | O_EXCL);
            if (r >= 0) { close(r); r = 0; }
        }
        if (r < 0) {
            char msg[400];
            snprintf(msg, sizeof(msg), "Could not create \"%s\": %s", name, strerror(errno));
            ui_msgbox(win, "Files", msg, "OK");
        } else {
            refresh();
            select_name(name);
        }
    }
    free(name);
}

static void do_copy(bool cut) {
    entry_t *e = selected();
    if (!e) return;
    join(clip_path, sizeof(clip_path), cwd, e->name);
    clip_cut = cut;
}

static void do_paste(void) {
    if (!clip_path[0]) return;
    const char *base = ui_basename(clip_path);
    char dst[512];
    join(dst, sizeof(dst), cwd, base);
    /* pasting into the same folder: make a copy name */
    struct stat st;
    if (stat(dst, &st) == 0) {
        if (clip_cut) { if (!strcmp(dst, clip_path)) { clip_path[0] = 0; return; } }
        char name[300];
        const char *dot = strrchr(base, '.');
        for (int i = 1; i < 100; i++) {
            if (dot && dot != base) snprintf(name, sizeof(name), "%.*s (copy %d)%s", (int)(dot - base), base, i, dot);
            else snprintf(name, sizeof(name), "%s (copy %d)", base, i);
            join(dst, sizeof(dst), cwd, name);
            if (stat(dst, &st) != 0) break;
        }
    }
    int r;
    if (clip_cut) {
        r = rename(clip_path, dst);
        if (r < 0 && errno == EXDEV) {
            r = copy_tree(clip_path, dst);
            if (r == 0) delete_tree(clip_path);
        }
        if (r == 0) clip_path[0] = 0;
    } else {
        r = copy_tree(clip_path, dst);
    }
    if (r < 0) {
        char msg[400];
        snprintf(msg, sizeof(msg), "Could not paste: %s", strerror(errno));
        ui_msgbox(win, "Files", msg, "OK");
    }
    refresh();
    select_name(ui_basename(dst));
}

static void do_properties(void) {
    entry_t *e = selected();
    if (!e) return;
    char full[512], msg[700], sz[32], tb[64];
    join(full, sizeof(full), cwd, e->name);
    format_size(e->size, sz, sizeof(sz));
    strftime(tb, sizeof(tb), "%A, %d %B %Y %H:%M", localtime(&e->mtime));
    snprintf(msg, sizeof(msg), "Name: %s\nType: %s\nLocation: %s\nSize: %s (%ld bytes)\nModified: %s", e->name,
             type_for(e), cwd, e->dir ? "-" : sz, e->dir ? 0L : e->size, tb);
    ui_msgbox(win, "Properties", msg, "OK");
}

/* ------------------------------------------------------------------ callbacks */
enum { M_OPEN = 1, M_EDIT, M_RENAME, M_DELETE, M_COPY, M_CUT, M_PASTE, M_NEWFOLDER, M_NEWFILE, M_TERMINAL,
       M_REFRESH, M_PROPS };

static void menu_cb(ui_window_t *w, int id) {
    (void)w;
    entry_t *e = selected();
    switch (id) {
    case M_OPEN: if (e) open_entry(e, false); break;
    case M_EDIT: if (e) open_entry(e, true); break;
    case M_RENAME: do_rename(); break;
    case M_DELETE: do_delete(); break;
    case M_COPY: do_copy(false); break;
    case M_CUT: do_copy(true); break;
    case M_PASTE: do_paste(); break;
    case M_NEWFOLDER: do_new(true); break;
    case M_NEWFILE: do_new(false); break;
    case M_TERMINAL: {
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "cd '%s'; sh", cwd);
        gui_launch("/bin/terminal", cmd);
        break;
    }
    case M_REFRESH: refresh(); break;
    case M_PROPS: do_properties(); break;
    }
}

static void on_activate(ui_widget_t *w) {
    (void)w;
    entry_t *e = selected();
    if (e) open_entry(e, false);
}

static void on_select(ui_widget_t *w) { (void)w; update_status(); }

static void on_place(ui_widget_t *w) {
    int i = w->ival;
    if (i >= 0 && i < (int)(sizeof(place_list) / sizeof(place_list[0]))) load_dir(place_list[i].path, true);
}

static void go_back(ui_widget_t *w) { (void)w; if (hist_pos > 0) { hist_pos--; load_dir(history[hist_pos], false); } }
static void go_fwd(ui_widget_t *w) { (void)w; if (hist_pos < hist_len - 1) { hist_pos++; load_dir(history[hist_pos], false); } }
static void go_up(ui_widget_t *w) {
    (void)w;
    if (!strcmp(cwd, "/")) return;
    char p[256], child[256];
    strlcpy(p, cwd, sizeof(p));
    char *s = strrchr(p, '/');
    strlcpy(child, s + 1, sizeof(child));
    if (s == p) p[1] = 0; else *s = 0;
    load_dir(p, true);
    select_name(child);
}
static void do_refresh(ui_widget_t *w) { (void)w; refresh(); }
static void toggle_view(ui_widget_t *w) {
    grid_view = !grid_view;
    strlcpy(w->glyph, grid_view ? "list" : "grid", sizeof(w->glyph));
    int sel = list->ival;
    fill_list();
    ui_list_select(list, sel);
}
static void path_enter(ui_widget_t *w) { load_dir(ui_get_text(w), true); }
static void tb_newfolder(ui_widget_t *w) { (void)w; do_new(true); }
static void tb_delete(ui_widget_t *w) { (void)w; do_delete(); }

static void on_key(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    bool ctrl = ev->mods & MOD_CTRL;
    if (ev->key == KEY_DELETE) do_delete();
    else if (ev->key == KEY_F2) do_rename();
    else if (ev->key == KEY_F5) refresh();
    else if (ev->key == KEY_BACKSPACE) go_up(0);
    else if ((ev->mods & MOD_ALT) && ev->key == KEY_LEFT) go_back(0);
    else if ((ev->mods & MOD_ALT) && ev->key == KEY_RIGHT) go_fwd(0);
    else if (ctrl && (ev->ch == 'c' || ev->ch == 'C')) do_copy(false);
    else if (ctrl && (ev->ch == 'x' || ev->ch == 'X')) do_copy(true);
    else if (ctrl && (ev->ch == 'v' || ev->ch == 'V')) do_paste();
    else if (ctrl && (ev->ch == 'l' || ev->ch == 'L')) { ui_focus(win, path_box); ui_textbox_select_all(path_box); }
}

/* right click on the list: context menu */
static bool (*list_default_event)(ui_widget_t *, gui_event_t *);
static void on_event(ui_window_t *w, gui_event_t *ev) {
    if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_RIGHT && rect_contains(list->r, ev->x, ev->y)) {
        ui_menu_t *m = ui_menu_new();
        entry_t *e = selected();
        if (e) {
            ui_menu_item(m, "Open", "Enter", M_OPEN);
            if (!e->dir) ui_menu_item(m, "Open with Text Editor", 0, M_EDIT);
            ui_menu_separator(m);
            ui_menu_item(m, "Cut", "Ctrl+X", M_CUT);
            ui_menu_item(m, "Copy", "Ctrl+C", M_COPY);
            ui_menu_item(m, "Paste", "Ctrl+V", M_PASTE);
            ui_menu_enable(m, M_PASTE, clip_path[0] != 0);
            ui_menu_separator(m);
            ui_menu_item(m, "Rename", "F2", M_RENAME);
            ui_menu_item(m, "Delete", "Del", M_DELETE);
            ui_menu_separator(m);
            ui_menu_item(m, "Properties", 0, M_PROPS);
        } else {
            ui_menu_item(m, "New Folder", 0, M_NEWFOLDER);
            ui_menu_item(m, "New Text File", 0, M_NEWFILE);
            ui_menu_separator(m);
            ui_menu_item(m, "Paste", "Ctrl+V", M_PASTE);
            ui_menu_enable(m, M_PASTE, clip_path[0] != 0);
            ui_menu_item(m, "Open Terminal Here", 0, M_TERMINAL);
            ui_menu_item(m, "Refresh", "F5", M_REFRESH);
        }
        ui_menu_popup(w, m, ev->x, ev->y, menu_cb);
    }
    (void)list_default_event;
}

static void paint(ui_window_t *w, surface_t *s) {
    /* toolbar background and sidebar */
    gfx_fill(s, 0, 0, w->w, TOOLBAR_H, ui_is_retro() ? ui_theme.window_bg : ui_theme.title_bg);
    gfx_hline(s, 0, TOOLBAR_H - 1, w->w, ui_theme.input_border);
    gfx_fill(s, 0, TOOLBAR_H, SIDEBAR_W, w->h - TOOLBAR_H - STATUS_H, ui_theme.sidebar_bg);
    gfx_vline(s, SIDEBAR_W - 1, TOOLBAR_H, w->h - TOOLBAR_H - STATUS_H, ui_theme.input_border);
    gfx_fill(s, 0, w->h - STATUS_H, w->w, STATUS_H, ui_is_retro() ? ui_theme.window_bg : ui_theme.title_bg);
    gfx_hline(s, 0, w->h - STATUS_H, w->w, ui_theme.input_border);
    font_draw(s, &ui_font_bold, 14, TOOLBAR_H + 10, "Places", ui_theme.window_text_dim);
}

int main(int argc, char **argv) {
    const char *start = argc > 1 && argv[1][0] ? argv[1] : "/home";
    int W = 800, H = 520;
    win = ui_window("Files", W, H, WF_RESIZABLE, "folder");
    if (!win) return 1;
    win->on_paint = paint;
    win->on_key = on_key;
    win->on_event = on_event;
    btn_back = ui_toolbutton(win, 8, 8, 32, "back", "Back", go_back);
    btn_fwd = ui_toolbutton(win, 42, 8, 32, "forward", "Forward", go_fwd);
    btn_up = ui_toolbutton(win, 76, 8, 32, "up", "Up", go_up);
    ui_toolbutton(win, 110, 8, 32, "refresh", "Refresh", do_refresh);
    path_box = ui_textbox(win, 150, 8, W - 150 - 124, 32, "");
    ui_set_anchor(path_box, A_LEFT | A_RIGHT | A_TOP);
    path_box->on_activate = path_enter;
    ui_widget_t *nf = ui_toolbutton(win, W - 116, 8, 32, "newfolder", "New folder", tb_newfolder);
    ui_set_anchor(nf, A_RIGHT | A_TOP);
    ui_widget_t *del = ui_toolbutton(win, W - 80, 8, 32, "delete", "Delete", tb_delete);
    ui_set_anchor(del, A_RIGHT | A_TOP);
    btn_view = ui_toolbutton(win, W - 44, 8, 32, "list", "View", toggle_view);
    ui_set_anchor(btn_view, A_RIGHT | A_TOP);

    places = ui_list(win, 8, TOOLBAR_H + 32, SIDEBAR_W - 16, 6 * 32 + 4);
    ui_list_set_row_height(places, 32);
    for (int i = 0; i < (int)(sizeof(place_list) / sizeof(place_list[0])); i++)
        ui_list_add(places, place_list[i].name, place_list[i].icon, 0);
    places->on_change = on_place;
    places->focusable = false;

    list = ui_list(win, SIDEBAR_W + 8, TOOLBAR_H + 8, W - SIDEBAR_W - 16, H - TOOLBAR_H - STATUS_H - 16);
    ui_set_anchor(list, A_ALL);
    list->on_activate = on_activate;
    list->on_change = on_select;
    status = ui_label(win, 12, H - STATUS_H + 1, W - 24, STATUS_H - 2, "");
    ui_set_anchor(status, A_LEFT | A_RIGHT | A_BOTTOM);
    status->color = 0;
    load_dir(start, true);
    ui_focus(win, list);
    ui_run();
    return 0;
}
