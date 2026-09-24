/* Text Editor */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <claudeos.h>

#define MENU_H 28
#define STATUS_H 24

typedef struct { char *s; int len, cap; } line_t;

static ui_window_t *win;
static ui_widget_t *canvas, *menubar, *status;
static ui_menu_t *m_view;
static line_t *lines;
static int nlines, lines_cap;
static int cur_l, cur_c;          /* cursor: line, byte offset */
static int anc_l, anc_c;          /* selection anchor */
static int want_x = -1;           /* preferred pixel column for vertical moves */
static int scroll_y, scroll_x;    /* first visible line, horizontal pixel scroll */
static char path[256];
static bool modified;
static bool show_numbers = true;
static bool dragging;
static char find_text[128];
static int cw, ch;                /* cell width, line height */

/* undo: snapshots of the whole text */
#define UNDO_MAX 60
typedef struct { char *text; int l, c; } snap_t;
static snap_t undo_stack[UNDO_MAX], redo_stack[UNDO_MAX];
static int nundo, nredo;
static int last_edit_kind;        /* merge consecutive typing into one undo step */

static void update_title(void);
static void update_status(void);

/* ------------------------------------------------------------------ text buffer */
static void line_init(line_t *l, const char *s, int n) {
    l->cap = n + 16;
    l->s = malloc(l->cap);
    memcpy(l->s, s, n);
    l->len = n;
    l->s[n] = 0;
}

static void line_reserve(line_t *l, int n) {
    if (n + 1 > l->cap) { l->cap = n + 32 + l->cap / 2; l->s = realloc(l->s, l->cap); }
}

static void insert_line(int at, const char *s, int n) {
    if (nlines == lines_cap) { lines_cap = lines_cap ? lines_cap * 2 : 64; lines = realloc(lines, sizeof(line_t) * lines_cap); }
    memmove(lines + at + 1, lines + at, sizeof(line_t) * (nlines - at));
    line_init(&lines[at], s, n);
    nlines++;
}

static void delete_line(int at) {
    free(lines[at].s);
    memmove(lines + at, lines + at + 1, sizeof(line_t) * (nlines - at - 1));
    nlines--;
}

static void clear_text(void) {
    for (int i = 0; i < nlines; i++) free(lines[i].s);
    nlines = 0;
}

static void set_text(const char *t) {
    clear_text();
    const char *p = t;
    while (1) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        if (n && p[n - 1] == '\r') n--;
        insert_line(nlines, p, n);
        if (!nl) break;
        p = nl + 1;
    }
    if (!nlines) insert_line(0, "", 0);
    cur_l = cur_c = anc_l = anc_c = 0;
    scroll_y = scroll_x = 0;
}

static char *get_text(size_t *out_len) {
    size_t total = 0;
    for (int i = 0; i < nlines; i++) total += lines[i].len + 1;
    char *buf = malloc(total + 1), *p = buf;
    for (int i = 0; i < nlines; i++) {
        memcpy(p, lines[i].s, lines[i].len);
        p += lines[i].len;
        if (i < nlines - 1) *p++ = '\n';
    }
    *p = 0;
    if (out_len) *out_len = p - buf;
    return buf;
}

/* ------------------------------------------------------------------ undo */
static void free_stack(snap_t *st, int *n) {
    for (int i = 0; i < *n; i++) free(st[i].text);
    *n = 0;
}

static void push_snap(snap_t *st, int *n, char *text, int l, int c) {
    if (*n == UNDO_MAX) { free(st[0].text); memmove(st, st + 1, sizeof(snap_t) * (UNDO_MAX - 1)); (*n)--; }
    st[*n].text = text;
    st[*n].l = l;
    st[*n].c = c;
    (*n)++;
}

static void save_undo(int kind) {
    if (kind && kind == last_edit_kind) return;
    last_edit_kind = kind;
    push_snap(undo_stack, &nundo, get_text(0), cur_l, cur_c);
    free_stack(redo_stack, &nredo);
}

static void restore(snap_t *from, int *nfrom, snap_t *to, int *nto) {
    if (!*nfrom) return;
    push_snap(to, nto, get_text(0), cur_l, cur_c);
    snap_t s = from[--(*nfrom)];
    set_text(s.text);
    cur_l = MIN(s.l, nlines - 1);
    cur_c = MIN(s.c, lines[cur_l].len);
    anc_l = cur_l;
    anc_c = cur_c;
    free(s.text);
    modified = true;
    last_edit_kind = 0;
    update_title();
}

/* ------------------------------------------------------------------ selection helpers */
static bool has_sel(void) { return cur_l != anc_l || cur_c != anc_c; }

static void sel_range(int *l0, int *c0, int *l1, int *c1) {
    if (anc_l < cur_l || (anc_l == cur_l && anc_c < cur_c)) { *l0 = anc_l; *c0 = anc_c; *l1 = cur_l; *c1 = cur_c; }
    else { *l0 = cur_l; *c0 = cur_c; *l1 = anc_l; *c1 = anc_c; }
}

static char *sel_text(size_t *len) {
    int l0, c0, l1, c1;
    sel_range(&l0, &c0, &l1, &c1);
    size_t total = 0;
    for (int l = l0; l <= l1; l++) total += lines[l].len + 1;
    char *buf = malloc(total + 1), *p = buf;
    for (int l = l0; l <= l1; l++) {
        int from = l == l0 ? c0 : 0, to = l == l1 ? c1 : lines[l].len;
        memcpy(p, lines[l].s + from, to - from);
        p += to - from;
        if (l != l1) *p++ = '\n';
    }
    *p = 0;
    *len = p - buf;
    return buf;
}

static void delete_sel(void) {
    if (!has_sel()) return;
    int l0, c0, l1, c1;
    sel_range(&l0, &c0, &l1, &c1);
    line_t *a = &lines[l0], *b = &lines[l1];
    int tail = b->len - c1;
    line_reserve(a, c0 + tail);
    memmove(a->s + c0, b->s + c1, tail + 1);
    a->len = c0 + tail;
    for (int l = l1; l > l0; l--) delete_line(l);
    cur_l = anc_l = l0;
    cur_c = anc_c = c0;
}

static void insert_text(const char *t, size_t n) {
    delete_sel();
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (c == '\r') continue;
        line_t *l = &lines[cur_l];
        if (c == '\n') {
            insert_line(cur_l + 1, l->s + cur_c, l->len - cur_c);
            l = &lines[cur_l];
            l->len = cur_c;
            l->s[cur_c] = 0;
            cur_l++;
            cur_c = 0;
            continue;
        }
        line_reserve(l, l->len + 1);
        memmove(l->s + cur_c + 1, l->s + cur_c, l->len - cur_c + 1);
        l->s[cur_c++] = c;
        l->len++;
    }
    anc_l = cur_l;
    anc_c = cur_c;
    modified = true;
}

/* ------------------------------------------------------------------ geometry */
static int gutter_w(void) {
    if (!show_numbers) return 8;
    int digits = 1;
    for (int n = nlines; n >= 10; n /= 10) digits++;
    return MAX(3, digits) * cw + 16;
}

static int visible_lines(void) { return MAX(1, (canvas->r.h - 4) / ch); }

static int col_to_x(int l, int c) { return font_text_width_n(&ui_font_mono, lines[l].s, c); }

static int x_to_col(int l, int x) { return font_index_at(&ui_font_mono, lines[l].s, x); }

static void ensure_visible(void) {
    int vis = visible_lines();
    if (cur_l < scroll_y) scroll_y = cur_l;
    if (cur_l >= scroll_y + vis) scroll_y = cur_l - vis + 1;
    int x = col_to_x(cur_l, cur_c);
    int textw = canvas->r.w - gutter_w() - 12;
    if (x - scroll_x > textw - cw * 2) scroll_x = x - textw + cw * 4;
    if (x - scroll_x < 0) scroll_x = MAX(0, x - cw * 4);
}

/* ------------------------------------------------------------------ drawing */
static void draw(ui_widget_t *w, surface_t *s) {
    uint32_t bg = ui_theme.input_bg, fg = ui_theme.input_text;
    gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, bg);
    int gw = gutter_w();
    if (show_numbers) gfx_fill(s, w->r.x, w->r.y, gw - 6, w->r.h, ui_theme.sidebar_bg);
    int vis = visible_lines() + 1;
    int l0, c0, l1, c1;
    sel_range(&l0, &c0, &l1, &c1);
    bool sel = has_sel();
    surface_t text = *s;
    rect_t tclip;
    rect_intersect(s->clip, mkrect(w->r.x + gw, w->r.y, w->r.w - gw, w->r.h), &tclip);
    gfx_set_clip(&text, tclip);
    for (int i = 0; i < vis; i++) {
        int l = scroll_y + i;
        if (l >= nlines) break;
        int y = w->r.y + 2 + i * ch;
        int tx = w->r.x + gw - scroll_x;
        if (l == cur_l && !sel) gfx_fill(&text, w->r.x + gw, y, w->r.w - gw, ch, WITH_ALPHA(ui_theme.accent, 22));
        if (sel && l >= l0 && l <= l1) {
            int from = l == l0 ? col_to_x(l, c0) : 0;
            int to = l == l1 ? col_to_x(l, c1) : col_to_x(l, lines[l].len) + cw;
            gfx_fill(&text, tx + from, y, to - from, ch, WITH_ALPHA(ui_theme.selection_bg, win->focused ? 110 : 60));
        }
        font_draw_n(&text, &ui_font_mono, tx, y, lines[l].s, lines[l].len, fg);
        if (show_numbers) {
            char num[16];
            snprintf(num, sizeof(num), "%d", l + 1);
            int nw = font_text_width(&ui_font_mono, num);
            font_draw(s, &ui_font_mono, w->r.x + gw - 14 - nw, y, num,
                      l == cur_l ? ui_theme.window_text : ui_theme.window_text_dim);
        }
        if (l == cur_l && win->focused) gfx_fill(&text, tx + col_to_x(l, cur_c), y, 2, ch, ui_theme.accent);
    }
    /* scrollbar */
    ui_draw_scrollbar(s, mkrect(w->r.x + w->r.w - 12, w->r.y, 12, w->r.h), nlines + visible_lines() - 1,
                      visible_lines(), scroll_y, false);
}

static void update_status(void) {
    char buf[160];
    int col = 1;
    for (int i = 0; i < cur_c; i = utf8_next(lines[cur_l].s, i)) col++;
    if (has_sel()) {
        size_t n;
        char *t = sel_text(&n);
        snprintf(buf, sizeof(buf), "Ln %d, Col %d  \xE2\x80\xA2  %d characters selected  \xE2\x80\xA2  %d lines  \xE2\x80\xA2  UTF-8",
                 cur_l + 1, col, utf8_len(t), nlines);
        free(t);
    } else {
        snprintf(buf, sizeof(buf), "Ln %d, Col %d  \xE2\x80\xA2  %d lines  \xE2\x80\xA2  UTF-8", cur_l + 1, col, nlines);
    }
    ui_set_text(status, buf);
}

static void update_title(void) {
    char t[300];
    snprintf(t, sizeof(t), "%s%s - Text Editor", modified ? "\xE2\x80\xA2 " : "", path[0] ? ui_basename(path) : "Untitled");
    ui_set_title(win, t);
}

static void changed(void) {
    ensure_visible();
    ui_widget_invalidate(canvas);
    update_status();
    update_title();
}

/* ------------------------------------------------------------------ file handling */
static bool save_as(void);

static bool save_to(const char *p) {
    size_t n;
    char *t = get_text(&n);
    bool ok = ui_write_file(p, t, n);
    free(t);
    if (!ok) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Could not save \"%s\".", p);
        ui_msgbox(win, "Save", msg, "OK");
        return false;
    }
    strlcpy(path, p, sizeof(path));
    modified = false;
    update_title();
    return true;
}

static bool save(void) { return path[0] ? save_to(path) : save_as(); }

static bool save_as(void) {
    char dir[256] = "/home";
    if (path[0]) {
        strlcpy(dir, path, sizeof(dir));
        char *s = strrchr(dir, '/');
        if (s && s != dir) *s = 0;
    }
    char *p = ui_file_dialog(win, true, "Save As", dir, path[0] ? ui_basename(path) : "Untitled.txt", 0);
    if (!p) return false;
    bool r = save_to(p);
    free(p);
    return r;
}

/* returns false if the user cancelled */
static bool confirm_discard(void) {
    if (!modified) return true;
    char msg[300];
    snprintf(msg, sizeof(msg), "Do you want to save the changes to \"%s\"?", path[0] ? ui_basename(path) : "Untitled");
    int r = ui_msgbox(win, "Text Editor", msg, "Save|Don't Save|Cancel");
    if (r == 0) return save();
    return r == 1;
}

static void load(const char *p) {
    size_t n;
    char *t = ui_read_file(p, &n);
    if (!t) {
        /* a new file */
        set_text("");
        strlcpy(path, p, sizeof(path));
        modified = false;
        changed();
        return;
    }
    set_text(t);
    free(t);
    strlcpy(path, p, sizeof(path));
    modified = false;
    free_stack(undo_stack, &nundo);
    free_stack(redo_stack, &nredo);
    changed();
}

static void open_file(void) {
    if (!confirm_discard()) return;
    char dir[256] = "/home";
    if (path[0]) {
        strlcpy(dir, path, sizeof(dir));
        char *s = strrchr(dir, '/');
        if (s && s != dir) *s = 0;
    }
    char *p = ui_file_dialog(win, false, "Open", dir, 0, 0);
    if (p) { load(p); free(p); }
}

/* ------------------------------------------------------------------ editing commands */
static void copy_sel(bool cut) {
    if (!has_sel()) return;
    size_t n;
    char *t = sel_text(&n);
    gui_clipboard_set(t, n);
    free(t);
    if (cut) { save_undo(0); delete_sel(); changed(); }
}

static void paste(void) {
    long n = gui_clipboard_get(0, 0);
    if (n <= 0) return;
    char *buf = malloc(n + 1);
    gui_clipboard_get(buf, n);
    buf[n] = 0;
    save_undo(0);
    insert_text(buf, n);
    free(buf);
    last_edit_kind = 0;
    changed();
}

static void select_all(void) {
    anc_l = 0;
    anc_c = 0;
    cur_l = nlines - 1;
    cur_c = lines[cur_l].len;
    changed();
}

static bool find_next(bool from_start) {
    if (!find_text[0]) return false;
    int sl = from_start ? 0 : cur_l, sc = from_start ? 0 : cur_c;
    for (int pass = 0; pass < 2; pass++) {
        for (int l = sl; l < nlines; l++) {
            const char *hit = strcasestr(lines[l].s + (l == sl ? sc : 0), find_text);
            if (hit) {
                anc_l = cur_l = l;
                anc_c = (int)(hit - lines[l].s);
                cur_c = anc_c + (int)strlen(find_text);
                changed();
                return true;
            }
        }
        sl = 0;
        sc = 0;
    }
    return false;
}

static void do_find(void) {
    char *t = ui_input(win, "Find", "Find what:", find_text);
    if (!t) return;
    strlcpy(find_text, t, sizeof(find_text));
    free(t);
    if (!find_next(false)) {
        char msg[200];
        snprintf(msg, sizeof(msg), "Cannot find \"%s\".", find_text);
        ui_msgbox(win, "Find", msg, "OK");
    }
}

static void do_goto(void) {
    char *t = ui_input(win, "Go to Line", "Line number:", "");
    if (!t) return;
    int l = atoi(t) - 1;
    free(t);
    if (l < 0) return;
    cur_l = anc_l = MIN(l, nlines - 1);
    cur_c = anc_c = 0;
    changed();
}

static void new_doc(void) {
    if (!confirm_discard()) return;
    set_text("");
    path[0] = 0;
    modified = false;
    free_stack(undo_stack, &nundo);
    free_stack(redo_stack, &nredo);
    changed();
}

enum { M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_EXIT, M_UNDO, M_REDO, M_CUT, M_COPY, M_PASTE, M_SELALL, M_FIND,
       M_FINDNEXT, M_GOTO, M_NUMBERS, M_ABOUT, M_INSDATE };

static void menu_cb(ui_window_t *w, int id) {
    (void)w;
    switch (id) {
    case M_NEW: new_doc(); break;
    case M_OPEN: open_file(); break;
    case M_SAVE: save(); break;
    case M_SAVEAS: save_as(); break;
    case M_EXIT: if (confirm_discard()) { ui_window_close(win); ui_quit(); } break;
    case M_UNDO: restore(undo_stack, &nundo, redo_stack, &nredo); changed(); break;
    case M_REDO: restore(redo_stack, &nredo, undo_stack, &nundo); changed(); break;
    case M_CUT: copy_sel(true); break;
    case M_COPY: copy_sel(false); break;
    case M_PASTE: paste(); break;
    case M_SELALL: select_all(); break;
    case M_FIND: do_find(); break;
    case M_FINDNEXT: if (!find_next(false)) do_find(); break;
    case M_GOTO: do_goto(); break;
    case M_NUMBERS:
        show_numbers = !show_numbers;
        ui_menu_set_checked(m_view, M_NUMBERS, show_numbers);
        changed();
        break;
    case M_INSDATE: {
        char buf[64];
        time_t t = time(0);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", localtime(&t));
        save_undo(0);
        insert_text(buf, strlen(buf));
        changed();
        break;
    }
    case M_ABOUT:
        ui_msgbox(win, "About Text Editor", "Text Editor for ClaudeOS\n\nA simple UTF-8 text editor with undo, find and clipboard support.", "OK");
        break;
    }
}

/* ------------------------------------------------------------------ input */
static void move_to(int l, int c, bool extend) {
    cur_l = MAX(0, MIN(l, nlines - 1));
    cur_c = MAX(0, MIN(c, lines[cur_l].len));
    if (!extend) { anc_l = cur_l; anc_c = cur_c; }
    last_edit_kind = 0;
}

static void pos_from_mouse(int x, int y, int *l, int *c) {
    int gl = scroll_y + (y - canvas->r.y - 2) / ch;
    *l = MAX(0, MIN(gl, nlines - 1));
    *c = x_to_col(*l, x - canvas->r.x - gutter_w() + scroll_x);
}

static bool event(ui_widget_t *w, gui_event_t *ev) {
    switch (ev->type) {
    case EV_MOUSE_WHEEL:
        scroll_y = MAX(0, MIN(nlines - 1, scroll_y + ev->wheel * 3));
        ui_widget_invalidate(w);
        return true;
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            int l, c;
            pos_from_mouse(ev->x, ev->y, &l, &c);
            if (ev->clicks == 2) {
                /* select word */
                const char *s = lines[l].s;
                int a = c, b = c;
                while (a > 0 && (isalnum((unsigned char)s[a - 1]) || s[a - 1] == '_' || (s[a - 1] & 0x80))) a--;
                while (b < lines[l].len && (isalnum((unsigned char)s[b]) || s[b] == '_' || (s[b] & 0x80))) b++;
                anc_l = cur_l = l;
                anc_c = a;
                cur_c = b;
            } else if (ev->clicks >= 3) {
                anc_l = cur_l = l;
                anc_c = 0;
                cur_c = lines[l].len;
            } else {
                move_to(l, c, ev->mods & MOD_SHIFT);
                dragging = true;
            }
            want_x = -1;
            changed();
        } else if (ev->button == MOUSE_RIGHT) {
            ui_menu_t *m = ui_menu_new();
            ui_menu_item(m, "Undo", "Ctrl+Z", M_UNDO);
            ui_menu_separator(m);
            ui_menu_item(m, "Cut", "Ctrl+X", M_CUT);
            ui_menu_item(m, "Copy", "Ctrl+C", M_COPY);
            ui_menu_item(m, "Paste", "Ctrl+V", M_PASTE);
            ui_menu_separator(m);
            ui_menu_item(m, "Select All", "Ctrl+A", M_SELALL);
            ui_menu_enable(m, M_CUT, has_sel());
            ui_menu_enable(m, M_COPY, has_sel());
            ui_menu_enable(m, M_UNDO, nundo > 0);
            ui_menu_popup(win, m, ev->x, ev->y, menu_cb);
        }
        return true;
    case EV_MOUSE_MOVE:
        if (dragging && (ev->buttons & MOUSE_LEFT)) {
            int l, c;
            pos_from_mouse(ev->x, ev->y, &l, &c);
            cur_l = l;
            cur_c = c;
            changed();
        }
        return true;
    case EV_MOUSE_UP:
        dragging = false;
        return true;
    case EV_KEY_DOWN: break;
    default: return false;
    }
    bool shift = ev->mods & MOD_SHIFT, ctrl = ev->mods & MOD_CTRL;
    uint32_t c = ev->ch;
    if (ctrl && !(ev->mods & MOD_ALTGR)) {
        switch (c | 0x20) {
        case 's': if (shift) save_as(); else save(); return true;
        case 'o': open_file(); return true;
        case 'n': new_doc(); return true;
        case 'z': menu_cb(win, M_UNDO); return true;
        case 'y': menu_cb(win, M_REDO); return true;
        case 'c': copy_sel(false); return true;
        case 'x': copy_sel(true); return true;
        case 'v': paste(); return true;
        case 'a': select_all(); return true;
        case 'f': do_find(); return true;
        case 'g': do_goto(); return true;
        case 'q': menu_cb(win, M_EXIT); return true;
        }
    }
    int vis = visible_lines();
    switch (ev->key) {
    case KEY_LEFT:
        if (has_sel() && !shift) { int l0, c0, l1, c1; sel_range(&l0, &c0, &l1, &c1); move_to(l0, c0, false); }
        else if (cur_c > 0) move_to(cur_l, utf8_prev(lines[cur_l].s, cur_c), shift);
        else if (cur_l > 0) move_to(cur_l - 1, lines[cur_l - 1].len, shift);
        want_x = -1;
        changed();
        return true;
    case KEY_RIGHT:
        if (has_sel() && !shift) { int l0, c0, l1, c1; sel_range(&l0, &c0, &l1, &c1); move_to(l1, c1, false); }
        else if (cur_c < lines[cur_l].len) move_to(cur_l, utf8_next(lines[cur_l].s, cur_c), shift);
        else if (cur_l < nlines - 1) move_to(cur_l + 1, 0, shift);
        want_x = -1;
        changed();
        return true;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
        if (want_x < 0) want_x = col_to_x(cur_l, cur_c);
        int d = ev->key == KEY_UP ? -1 : ev->key == KEY_DOWN ? 1 : ev->key == KEY_PGUP ? -vis : vis;
        int nl = MAX(0, MIN(nlines - 1, cur_l + d));
        if (ev->key == KEY_PGUP || ev->key == KEY_PGDN) scroll_y = MAX(0, MIN(nlines - 1, scroll_y + d));
        move_to(nl, x_to_col(nl, want_x), shift);
        changed();
        return true;
    }
    case KEY_HOME: {
        /* smart home: first non-blank, then column 0 */
        int first = 0;
        while (first < lines[cur_l].len && (lines[cur_l].s[first] == ' ' || lines[cur_l].s[first] == '\t')) first++;
        if (ctrl) move_to(0, 0, shift);
        else move_to(cur_l, cur_c == first ? 0 : first, shift);
        want_x = -1;
        changed();
        return true;
    }
    case KEY_END:
        if (ctrl) move_to(nlines - 1, lines[nlines - 1].len, shift);
        else move_to(cur_l, lines[cur_l].len, shift);
        want_x = -1;
        changed();
        return true;
    case KEY_BACKSPACE:
        save_undo(2);
        if (has_sel()) delete_sel();
        else if (cur_c > 0) { anc_c = utf8_prev(lines[cur_l].s, cur_c); anc_l = cur_l; delete_sel(); }
        else if (cur_l > 0) { anc_l = cur_l - 1; anc_c = lines[cur_l - 1].len; delete_sel(); }
        modified = true;
        want_x = -1;
        changed();
        return true;
    case KEY_DELETE:
        save_undo(3);
        if (has_sel()) delete_sel();
        else if (cur_c < lines[cur_l].len) { anc_c = utf8_next(lines[cur_l].s, cur_c); anc_l = cur_l; delete_sel(); }
        else if (cur_l < nlines - 1) { anc_l = cur_l + 1; anc_c = 0; delete_sel(); }
        modified = true;
        changed();
        return true;
    case KEY_ENTER: case KEY_KPENTER: {
        save_undo(0);
        /* auto indent */
        char indent[64];
        int n = 0;
        while (n < 63 && n < lines[cur_l].len && (lines[cur_l].s[n] == ' ' || lines[cur_l].s[n] == '\t')) {
            indent[n] = lines[cur_l].s[n];
            n++;
        }
        insert_text("\n", 1);
        insert_text(indent, n);
        want_x = -1;
        changed();
        return true;
    }
    case KEY_TAB:
        save_undo(0);
        insert_text("    ", 4);
        changed();
        return true;
    case KEY_F3:
        if (!find_next(false)) do_find();
        return true;
    }
    if (c >= ' ' && !ctrl) {
        char enc[4];
        int n = utf8_encode(c, enc);
        save_undo(1);
        insert_text(enc, n);
        want_x = -1;
        changed();
        return true;
    }
    return false;
}

static bool on_close(ui_window_t *w) {
    (void)w;
    if (!confirm_discard()) return false;
    ui_quit();
    return true;
}

int main(int argc, char **argv) {
    ui_init();
    cw = ui_font_mono.cell ? ui_font_mono.cell : 8;
    ch = ui_font_mono.height + 2;
    int W = 780, H = 540;
    win = ui_window("Text Editor", W, H, WF_RESIZABLE, "editor");
    if (!win) return 1;
    win->on_close = on_close;
    menubar = ui_menubar(win, menu_cb);
    ui_menu_t *mf = ui_menu_new();
    ui_menu_item(mf, "New", "Ctrl+N", M_NEW);
    ui_menu_item(mf, "Open...", "Ctrl+O", M_OPEN);
    ui_menu_item(mf, "Save", "Ctrl+S", M_SAVE);
    ui_menu_item(mf, "Save As...", "Ctrl+Shift+S", M_SAVEAS);
    ui_menu_separator(mf);
    ui_menu_item(mf, "Exit", "Ctrl+Q", M_EXIT);
    ui_menubar_add(menubar, "File", mf);
    ui_menu_t *me = ui_menu_new();
    ui_menu_item(me, "Undo", "Ctrl+Z", M_UNDO);
    ui_menu_item(me, "Redo", "Ctrl+Y", M_REDO);
    ui_menu_separator(me);
    ui_menu_item(me, "Cut", "Ctrl+X", M_CUT);
    ui_menu_item(me, "Copy", "Ctrl+C", M_COPY);
    ui_menu_item(me, "Paste", "Ctrl+V", M_PASTE);
    ui_menu_item(me, "Select All", "Ctrl+A", M_SELALL);
    ui_menu_separator(me);
    ui_menu_item(me, "Find...", "Ctrl+F", M_FIND);
    ui_menu_item(me, "Find Next", "F3", M_FINDNEXT);
    ui_menu_item(me, "Go to Line...", "Ctrl+G", M_GOTO);
    ui_menu_item(me, "Insert Date/Time", 0, M_INSDATE);
    ui_menubar_add(menubar, "Edit", me);
    m_view = ui_menu_new();
    ui_menu_check(m_view, "Line Numbers", true, M_NUMBERS);
    ui_menubar_add(menubar, "View", m_view);
    ui_menu_t *mh = ui_menu_new();
    ui_menu_item(mh, "About Text Editor", 0, M_ABOUT);
    ui_menubar_add(menubar, "Help", mh);
    canvas = ui_canvas(win, 0, MENU_H, W, H - MENU_H - STATUS_H, draw, event);
    ui_set_anchor(canvas, A_ALL);
    status = ui_label(win, 10, H - STATUS_H, W - 20, STATUS_H, "");
    ui_set_anchor(status, A_LEFT | A_RIGHT | A_BOTTOM);
    set_text("");
    if (argc > 1 && argv[1][0]) load(argv[1]);
    changed();
    ui_focus(win, canvas);
    canvas->cursor = CUR_TEXT;
    ui_run();
    return 0;
}
