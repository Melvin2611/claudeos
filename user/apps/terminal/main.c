/* Terminal emulator: VT100/ANSI subset on top of a pseudo terminal running /bin/sh */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <claudeos.h>

#define SCROLLBACK 1000
#define PAD 6

typedef struct { uint32_t ch; uint8_t fg, bg, attr; } cell_t;
#define ATTR_BOLD 1
#define ATTR_REVERSE 2
#define ATTR_UNDERLINE 4
#define DEF_FG 7
#define DEF_BG 0

static const uint32_t palette[16] = {
    0xFF1B1D23, 0xFFE06C75, 0xFF98C379, 0xFFE5C07B, 0xFF61AFEF, 0xFFC678DD, 0xFF56B6C2, 0xFFD4D8DF,
    0xFF5C6370, 0xFFFF7A85, 0xFFB5E08F, 0xFFFFD68A, 0xFF7FC3FF, 0xFFDB8FF0, 0xFF6FD5E0, 0xFFFFFFFF,
};

static ui_window_t *win;
static ui_widget_t *canvas;
static int master = -1;
static int child_pid;
static int cols = 80, rows = 24;
static cell_t *lines[SCROLLBACK + 256];   /* ring of line pointers: history + screen */
static int nlines;                         /* total valid lines (>= rows) */
static int line_cap;
static int cx, cy;                         /* cursor (screen coordinates) */
static int saved_cx, saved_cy;
static uint8_t cur_fg = DEF_FG, cur_bg = DEF_BG, cur_attr;
static bool cursor_visible = true, blink_on = true;
static int view_off;                       /* lines scrolled back */
static int cell_w = 8, cell_h = 17;
static bool wrap_pending;
static int scroll_top, scroll_bottom;      /* scrolling region (screen rows) */

/* parser */
enum { ST_NORMAL, ST_ESC, ST_CSI, ST_OSC };
static int state;
static int params[16], nparams;
static bool csi_private;
static uint32_t utf8_cp;
static int utf8_left;

/* selection (absolute line index, column) */
static bool selecting, has_sel;
static int sel_l0, sel_c0, sel_l1, sel_c1;

static cell_t *new_line(void) {
    cell_t *l = malloc(sizeof(cell_t) * cols);
    for (int i = 0; i < cols; i++) l[i] = (cell_t){ ' ', DEF_FG, DEF_BG, 0 };
    return l;
}

static int first_screen_line(void) { return nlines - rows; }
static cell_t *screen_line(int y) { return lines[first_screen_line() + y]; }

static void init_buffer(void) {
    line_cap = SCROLLBACK + 256;
    nlines = rows;
    for (int i = 0; i < rows; i++) lines[i] = new_line();
    scroll_top = 0;
    scroll_bottom = rows - 1;
}

static void invalidate(void) { ui_widget_invalidate(canvas); }

/* scroll the region up by one line (new blank line at the bottom) */
static void scroll_up(void) {
    if (scroll_top == 0 && scroll_bottom == rows - 1) {
        if (nlines >= line_cap) {
            free(lines[0]);
            memmove(lines, lines + 1, sizeof(cell_t *) * (nlines - 1));
            nlines--;
            if (has_sel) { sel_l0--; sel_l1--; }
        }
        lines[nlines++] = new_line();
        return;
    }
    int base = first_screen_line();
    free(lines[base + scroll_top]);
    for (int y = scroll_top; y < scroll_bottom; y++) lines[base + y] = lines[base + y + 1];
    lines[base + scroll_bottom] = new_line();
}

static void scroll_down(void) {
    int base = first_screen_line();
    free(lines[base + scroll_bottom]);
    for (int y = scroll_bottom; y > scroll_top; y--) lines[base + y] = lines[base + y - 1];
    lines[base + scroll_top] = new_line();
}

static void newline(void) {
    if (cy == scroll_bottom) scroll_up();
    else if (cy < rows - 1) cy++;
}

static void put_char(uint32_t ch) {
    if (wrap_pending) {
        cx = 0;
        newline();
        wrap_pending = false;
    }
    cell_t *l = screen_line(cy);
    l[cx] = (cell_t){ ch, cur_fg, cur_bg, cur_attr };
    if (cx == cols - 1) wrap_pending = true;
    else cx++;
}

static void erase_line_part(int y, int from, int to) {
    cell_t *l = screen_line(y);
    for (int x = MAX(0, from); x < MIN(cols, to); x++) l[x] = (cell_t){ ' ', DEF_FG, cur_bg, 0 };
}

static void csi_dispatch(char c) {
    int p0 = nparams > 0 ? params[0] : 0;
    int p1 = nparams > 1 ? params[1] : 0;
    int n = p0 ? p0 : 1;
    wrap_pending = false;
    switch (c) {
    case 'A': cy = MAX(0, cy - n); break;
    case 'B': cy = MIN(rows - 1, cy + n); break;
    case 'C': cx = MIN(cols - 1, cx + n); break;
    case 'D': cx = MAX(0, cx - n); break;
    case 'E': cx = 0; cy = MIN(rows - 1, cy + n); break;
    case 'F': cx = 0; cy = MAX(0, cy - n); break;
    case 'G': case '`': cx = MIN(cols - 1, MAX(0, n - 1)); break;
    case 'd': cy = MIN(rows - 1, MAX(0, n - 1)); break;
    case 'H': case 'f':
        cy = MIN(rows - 1, MAX(0, (p0 ? p0 : 1) - 1));
        cx = MIN(cols - 1, MAX(0, (p1 ? p1 : 1) - 1));
        break;
    case 'J':
        if (p0 == 0) { erase_line_part(cy, cx, cols); for (int y = cy + 1; y < rows; y++) erase_line_part(y, 0, cols); }
        else if (p0 == 1) { erase_line_part(cy, 0, cx + 1); for (int y = 0; y < cy; y++) erase_line_part(y, 0, cols); }
        else { for (int y = 0; y < rows; y++) erase_line_part(y, 0, cols); if (p0 == 2) { cx = cy = 0; } }
        break;
    case 'K':
        if (p0 == 0) erase_line_part(cy, cx, cols);
        else if (p0 == 1) erase_line_part(cy, 0, cx + 1);
        else erase_line_part(cy, 0, cols);
        break;
    case 'X': erase_line_part(cy, cx, cx + n); break;
    case 'P': {
        cell_t *l = screen_line(cy);
        for (int x = cx; x < cols; x++) l[x] = x + n < cols ? l[x + n] : (cell_t){ ' ', DEF_FG, cur_bg, 0 };
        break;
    }
    case '@': {
        cell_t *l = screen_line(cy);
        for (int x = cols - 1; x >= cx; x--) l[x] = x - n >= cx ? l[x - n] : (cell_t){ ' ', DEF_FG, cur_bg, 0 };
        break;
    }
    case 'L': for (int i = 0; i < n; i++) { int t = scroll_top; scroll_top = cy; scroll_down(); scroll_top = t; } break;
    case 'M': for (int i = 0; i < n; i++) { int t = scroll_top; scroll_top = cy; scroll_up(); scroll_top = t; } break;
    case 'S': for (int i = 0; i < n; i++) scroll_up(); break;
    case 'T': for (int i = 0; i < n; i++) scroll_down(); break;
    case 'r':
        scroll_top = p0 ? p0 - 1 : 0;
        scroll_bottom = p1 ? p1 - 1 : rows - 1;
        if (scroll_top >= scroll_bottom || scroll_bottom >= rows) { scroll_top = 0; scroll_bottom = rows - 1; }
        cx = cy = 0;
        break;
    case 's': saved_cx = cx; saved_cy = cy; break;
    case 'u': cx = saved_cx; cy = saved_cy; break;
    case 'h': case 'l':
        if (csi_private && p0 == 25) cursor_visible = c == 'h';
        break;
    case 'm':
        if (nparams == 0) { cur_fg = DEF_FG; cur_bg = DEF_BG; cur_attr = 0; }
        for (int i = 0; i < nparams; i++) {
            int p = params[i];
            if (p == 0) { cur_fg = DEF_FG; cur_bg = DEF_BG; cur_attr = 0; }
            else if (p == 1) cur_attr |= ATTR_BOLD;
            else if (p == 4) cur_attr |= ATTR_UNDERLINE;
            else if (p == 7) cur_attr |= ATTR_REVERSE;
            else if (p == 22) cur_attr &= ~ATTR_BOLD;
            else if (p == 24) cur_attr &= ~ATTR_UNDERLINE;
            else if (p == 27) cur_attr &= ~ATTR_REVERSE;
            else if (p >= 30 && p <= 37) cur_fg = p - 30;
            else if (p == 39) cur_fg = DEF_FG;
            else if (p >= 40 && p <= 47) cur_bg = p - 40;
            else if (p == 49) cur_bg = DEF_BG;
            else if (p >= 90 && p <= 97) cur_fg = p - 90 + 8;
            else if (p >= 100 && p <= 107) cur_bg = p - 100 + 8;
            else if ((p == 38 || p == 48) && i + 2 < nparams && params[i + 1] == 5) {
                int idx = params[i + 2];
                if (idx < 16) { if (p == 38) cur_fg = idx; else cur_bg = idx; }
                i += 2;
            }
        }
        break;
    }
}

static void feed(const unsigned char *data, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = data[i];
        switch (state) {
        case ST_NORMAL:
            if (utf8_left) {
                if ((c & 0xC0) == 0x80) {
                    utf8_cp = (utf8_cp << 6) | (c & 0x3F);
                    if (--utf8_left == 0) put_char(utf8_cp);
                    continue;
                }
                utf8_left = 0;
                put_char(0xFFFD);
            }
            if (c == 0x1B) { state = ST_ESC; continue; }
            if (c == '\r') { cx = 0; wrap_pending = false; continue; }
            if (c == '\n' || c == 0x0B || c == 0x0C) { wrap_pending = false; newline(); continue; }
            if (c == '\b') { if (cx > 0) cx--; wrap_pending = false; continue; }
            if (c == '\t') { cx = MIN(cols - 1, (cx + 8) & ~7); continue; }
            if (c == 0x07) { beep(880, 40); continue; }
            if (c < 0x20) continue;
            if (c >= 0x80) {
                if ((c & 0xE0) == 0xC0) { utf8_cp = c & 0x1F; utf8_left = 1; }
                else if ((c & 0xF0) == 0xE0) { utf8_cp = c & 0x0F; utf8_left = 2; }
                else if ((c & 0xF8) == 0xF0) { utf8_cp = c & 0x07; utf8_left = 3; }
                else put_char(0xFFFD);
                continue;
            }
            put_char(c);
            break;
        case ST_ESC:
            if (c == '[') { state = ST_CSI; nparams = 0; params[0] = 0; csi_private = false; continue; }
            if (c == ']') { state = ST_OSC; continue; }
            if (c == '7') { saved_cx = cx; saved_cy = cy; }
            else if (c == '8') { cx = saved_cx; cy = saved_cy; }
            else if (c == 'c') { for (int y = 0; y < rows; y++) erase_line_part(y, 0, cols); cx = cy = 0; cur_fg = DEF_FG; cur_bg = DEF_BG; cur_attr = 0; }
            else if (c == 'M') { if (cy == scroll_top) scroll_down(); else if (cy > 0) cy--; }
            else if (c == 'D') newline();
            state = ST_NORMAL;
            break;
        case ST_CSI:
            if (c == '?') { csi_private = true; continue; }
            if (c >= '0' && c <= '9') {
                if (nparams == 0) nparams = 1;
                params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
                continue;
            }
            if (c == ';') {
                if (nparams == 0) nparams = 1;
                if (nparams < 16) params[nparams++] = 0;
                continue;
            }
            if (c >= 0x40 && c <= 0x7E) { csi_dispatch(c); state = ST_NORMAL; }
            break;
        case ST_OSC:
            if (c == 0x07) state = ST_NORMAL;
            else if (c == 0x1B) state = ST_ESC;
            break;
        }
    }
    view_off = 0;
    invalidate();
}

/* ------------------------------------------------------------------ drawing */
static bool in_selection(int line, int col) {
    if (!has_sel) return false;
    int l0 = sel_l0, c0 = sel_c0, l1 = sel_l1, c1 = sel_c1;
    if (l0 > l1 || (l0 == l1 && c0 > c1)) { int t = l0; l0 = l1; l1 = t; t = c0; c0 = c1; c1 = t; }
    if (line < l0 || line > l1) return false;
    if (line == l0 && col < c0) return false;
    if (line == l1 && col > c1) return false;
    return true;
}

static void draw(ui_widget_t *w, surface_t *s) {
    gfx_fill(s, w->r.x, w->r.y, w->r.w, w->r.h, palette[0]);
    int first = first_screen_line() - view_off;
    for (int y = 0; y < rows; y++) {
        int li = first + y;
        if (li < 0 || li >= nlines) continue;
        cell_t *l = lines[li];
        int py = w->r.y + PAD + y * cell_h;
        for (int x = 0; x < cols; x++) {
            cell_t c = l[x];
            uint8_t fg = c.fg, bg = c.bg;
            if ((c.attr & ATTR_BOLD) && fg < 8) fg += 8;
            if (c.attr & ATTR_REVERSE) { uint8_t t = fg; fg = bg; bg = t; }
            int px = w->r.x + PAD + x * cell_w;
            bool sel = in_selection(li, x);
            if (sel) { gfx_fill(s, px, py, cell_w, cell_h, 0xFF3E5F8A); }
            else if (bg != DEF_BG) gfx_fill(s, px, py, cell_w, cell_h, palette[bg]);
            if (c.ch != ' ') font_draw_cp(s, (c.attr & ATTR_BOLD) ? &ui_font_mono_bold : &ui_font_mono, px, py, c.ch, palette[fg]);
            if (c.attr & ATTR_UNDERLINE) gfx_hline(s, px, py + cell_h - 2, cell_w, palette[fg]);
        }
    }
    if (cursor_visible && view_off == 0 && blink_on) {
        int px = w->r.x + PAD + cx * cell_w, py = w->r.y + PAD + cy * cell_h;
        if (win->focused) gfx_fill(s, px, py, cell_w, cell_h, WITH_ALPHA(palette[7], 200));
        else gfx_rect(s, px, py, cell_w, cell_h, palette[7]);
        cell_t c = screen_line(cy)[cx];
        if (win->focused && c.ch != ' ') font_draw_cp(s, &ui_font_mono, px, py, c.ch, palette[0]);
    }
    if (nlines > rows) {
        rect_t track = mkrect(w->r.x + w->r.w - 10, w->r.y + 2, 10, w->r.h - 4);
        ui_draw_scrollbar(s, track, nlines, rows, first_screen_line() - view_off, false);
    }
}

/* ------------------------------------------------------------------ input */
static void send_str(const char *s, int n) {
    if (master >= 0) write(master, s, n);
    if (view_off) { view_off = 0; invalidate(); }
}

static void copy_selection(void) {
    if (!has_sel) return;
    int l0 = sel_l0, c0 = sel_c0, l1 = sel_l1, c1 = sel_c1;
    if (l0 > l1 || (l0 == l1 && c0 > c1)) { int t = l0; l0 = l1; l1 = t; t = c0; c0 = c1; c1 = t; }
    size_t cap = (size_t)(l1 - l0 + 1) * (cols * 4 + 1) + 1, n = 0;
    char *buf = malloc(cap);
    for (int l = l0; l <= l1 && l < nlines; l++) {
        int from = l == l0 ? c0 : 0, to = l == l1 ? c1 : cols - 1;
        int last = to;
        while (last >= from && lines[l][last].ch == ' ') last--;
        for (int x = from; x <= last; x++) n += utf8_encode(lines[l][x].ch, buf + n);
        if (l != l1) buf[n++] = '\n';
    }
    buf[n] = 0;
    gui_clipboard_set(buf, n);
    free(buf);
}

static void paste(void) {
    long n = gui_clipboard_get(0, 0);
    if (n <= 0) return;
    char *buf = malloc(n + 1);
    gui_clipboard_get(buf, n);
    for (long i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '\r';
    send_str(buf, (int)n);
    free(buf);
}

static void cell_at(int x, int y, int *line, int *col) {
    int c = (x - PAD) / cell_w, r = (y - PAD) / cell_h;
    c = MAX(0, MIN(cols - 1, c));
    r = MAX(0, MIN(rows - 1, r));
    *line = first_screen_line() - view_off + r;
    *col = c;
}

static void menu_cb(ui_window_t *w, int id) {
    (void)w;
    if (id == 1) copy_selection();
    else if (id == 2) paste();
    else if (id == 3) { send_str("\x0c", 1); }
    else if (id == 4) gui_launch("/bin/terminal", "");
}

static bool event(ui_widget_t *w, gui_event_t *ev) {
    int lx = ev->x - w->r.x, ly = ev->y - w->r.y;
    switch (ev->type) {
    case EV_MOUSE_WHEEL: {
        int maxoff = nlines - rows;
        view_off = MAX(0, MIN(maxoff, view_off - ev->wheel * 3));
        invalidate();
        return true;
    }
    case EV_MOUSE_DOWN:
        if (ev->button == MOUSE_LEFT) {
            cell_at(lx, ly, &sel_l0, &sel_c0);
            sel_l1 = sel_l0;
            sel_c1 = sel_c0;
            selecting = true;
            has_sel = false;
            invalidate();
        } else if (ev->button == MOUSE_MIDDLE) {
            paste();
        } else if (ev->button == MOUSE_RIGHT) {
            ui_menu_t *m = ui_menu_new();
            ui_menu_item(m, "Copy", "Ctrl+Shift+C", 1);
            ui_menu_item(m, "Paste", "Ctrl+Shift+V", 2);
            ui_menu_separator(m);
            ui_menu_item(m, "Clear", "Ctrl+L", 3);
            ui_menu_item(m, "New Window", 0, 4);
            ui_menu_enable(m, 1, has_sel);
            ui_menu_popup(win, m, ev->x, ev->y, menu_cb);
        }
        return true;
    case EV_MOUSE_MOVE:
        if (selecting) {
            cell_at(lx, ly, &sel_l1, &sel_c1);
            has_sel = sel_l1 != sel_l0 || sel_c1 != sel_c0;
            invalidate();
        }
        return true;
    case EV_MOUSE_UP:
        if (selecting && ev->button == MOUSE_LEFT) {
            selecting = false;
            if (has_sel) copy_selection();
        }
        return true;
    case EV_KEY_DOWN: {
        uint32_t m = ev->mods;
        if ((m & MOD_CTRL) && (m & MOD_SHIFT)) {
            if (ev->key == KEY_C) { copy_selection(); return true; }
            if (ev->key == KEY_V) { paste(); return true; }
        }
        if (m & MOD_SHIFT) {
            if (ev->key == KEY_PGUP) { view_off = MIN(nlines - rows, view_off + rows / 2); invalidate(); return true; }
            if (ev->key == KEY_PGDN) { view_off = MAX(0, view_off - rows / 2); invalidate(); return true; }
        }
        if (has_sel) { has_sel = false; invalidate(); }
        const char *seq = 0;
        switch (ev->key) {
        case KEY_UP: seq = "\x1b[A"; break;
        case KEY_DOWN: seq = "\x1b[B"; break;
        case KEY_RIGHT: seq = "\x1b[C"; break;
        case KEY_LEFT: seq = "\x1b[D"; break;
        case KEY_HOME: seq = "\x1b[H"; break;
        case KEY_END: seq = "\x1b[F"; break;
        case KEY_DELETE: seq = "\x1b[3~"; break;
        case KEY_INSERT: seq = "\x1b[2~"; break;
        case KEY_PGUP: seq = "\x1b[5~"; break;
        case KEY_PGDN: seq = "\x1b[6~"; break;
        case KEY_ENTER: case KEY_KPENTER: seq = "\r"; break;
        case KEY_BACKSPACE: seq = "\x7f"; break;
        case KEY_TAB: seq = "\t"; break;
        case KEY_ESC: seq = "\x1b"; break;
        }
        if (seq) { send_str(seq, (int)strlen(seq)); return true; }
        uint32_t ch = ev->ch;
        if ((m & MOD_CTRL) && !(m & MOD_ALT) && !(m & MOD_ALTGR)) {
            if (ch >= 'a' && ch <= 'z') { char c = (char)(ch - 'a' + 1); send_str(&c, 1); return true; }
            if (ch >= 'A' && ch <= 'Z') { char c = (char)(ch - 'A' + 1); send_str(&c, 1); return true; }
            return true;
        }
        if (ch >= ' ') {
            char buf[4];
            int n = utf8_encode(ch, buf);
            send_str(buf, n);
        }
        return true;
    }
    }
    return false;
}

/* ------------------------------------------------------------------ resize & I/O */
static void apply_size(void) {
    int ncols = MAX(20, (canvas->r.w - 2 * PAD) / cell_w);
    int nrows = MAX(5, (canvas->r.h - 2 * PAD) / cell_h);
    if (ncols == cols && nrows == rows) return;
    /* adjust existing lines to the new width */
    for (int i = 0; i < nlines; i++) {
        cell_t *l = malloc(sizeof(cell_t) * ncols);
        for (int x = 0; x < ncols; x++) l[x] = x < cols ? lines[i][x] : (cell_t){ ' ', DEF_FG, DEF_BG, 0 };
        free(lines[i]);
        lines[i] = l;
    }
    cols = ncols;
    /* height: add blank lines at the bottom or keep the cursor in view */
    while (nlines < nrows) {
        memmove(lines + 1, lines, sizeof(cell_t *) * nlines);
        lines[0] = new_line();
        nlines++;
        cy++;
    }
    if (nrows > rows) {
        int extra = nrows - rows;
        /* pull lines from history if there are any */
        int from_history = MIN(extra, first_screen_line());
        cy += from_history;
        for (int i = from_history; i < extra; i++) lines[nlines++] = new_line();
    } else if (nrows < rows) {
        int shrink = rows - nrows;
        /* drop empty lines below the cursor first */
        int below = rows - 1 - cy;
        int drop = MIN(shrink, below);
        for (int i = 0; i < drop; i++) free(lines[--nlines]);
        cy -= shrink - drop;
    }
    rows = nrows;
    cy = MAX(0, MIN(cy, rows - 1));
    cx = MIN(cx, cols - 1);
    scroll_top = 0;
    scroll_bottom = rows - 1;
    view_off = 0;
    has_sel = false;
    kwinsize_t ws = { (uint16_t)rows, (uint16_t)cols, 0, 0 };
    if (master >= 0) ioctl(master, TIOCSWINSZ, &ws);
    invalidate();
}

static void on_resize(ui_window_t *w) { (void)w; apply_size(); }

static void on_output(int fd, void *arg) {
    (void)arg;
    unsigned char buf[4096];
    int total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n == 0) { ui_unwatch_fd(fd); close(fd); master = -1; ui_window_close(win); ui_quit(); }
            return;
        }
        feed(buf, (int)n);
        total += n;
        if (n < (ssize_t)sizeof(buf) || total > 64 * 1024) break;
    }
}

static void blink(void *arg) {
    (void)arg;
    blink_on = !blink_on;
    if (cursor_visible && view_off == 0) {
        int px = canvas->r.x + PAD + cx * cell_w, py = canvas->r.y + PAD + cy * cell_h;
        ui_invalidate_rect(win, mkrect(px, py, cell_w, cell_h));
    }
}

static bool on_close(ui_window_t *w) {
    (void)w;
    if (child_pid > 0) kill(child_pid, SIGKILL);
    ui_quit();
    return true;
}

int main(int argc, char **argv) {
    ui_init();
    cell_w = ui_font_mono.cell ? ui_font_mono.cell : 8;
    cell_h = ui_font_mono.height;
    int W = 80 * cell_w + 2 * PAD, H = 24 * cell_h + 2 * PAD;
    win = ui_window("Terminal", W, H, WF_RESIZABLE, "terminal");
    if (!win) return 1;
    win->bg = palette[0];
    win->on_resize = on_resize;
    win->on_close = on_close;
    canvas = ui_canvas(win, 0, 0, W, H, draw, event);
    ui_set_anchor(canvas, A_ALL);
    ui_focus(win, canvas);
    ui_set_cursor(win, CUR_TEXT);
    init_buffer();

    master = open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        const char *msg = "error: cannot open /dev/ptmx\r\n";
        feed((const unsigned char *)msg, (int)strlen(msg));
        ui_run();
        return 1;
    }
    int slave;
    ioctl(master, TIOCPTYNEW, &slave);
    kwinsize_t ws = { (uint16_t)rows, (uint16_t)cols, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);
    int map[3] = { slave, slave, slave };
    char *sh_argv[4] = { "sh", 0, 0, 0 };
    if (argc > 1 && argv[1][0]) {
        /* terminal <command>: run it through the shell */
        sh_argv[1] = "-c";
        sh_argv[2] = argv[1];
    }
    child_pid = spawn("/bin/sh", sh_argv, environ, map, 0);
    close(slave);
    if (child_pid < 0) {
        const char *msg = "error: cannot start /bin/sh\r\n";
        feed((const unsigned char *)msg, (int)strlen(msg));
    }
    ui_watch_fd(master, on_output, 0);
    ui_timer(530, blink, 0);
    ui_run();
    return 0;
}
