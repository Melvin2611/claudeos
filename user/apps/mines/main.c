/* Minesweeper */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <claudeos.h>

#define CELL 28
#define TOP 64
#define MAXW 30
#define MAXH 16

static ui_window_t *win;
static ui_widget_t *board, *face_btn, *mines_lbl, *time_lbl;
static int bw = 9, bh = 9, nmines = 10;
static int8_t mine[MAXH][MAXW], adj[MAXH][MAXW], state[MAXH][MAXW];   /* state: 0 hidden 1 open 2 flag */
static bool started, over, won;
static int flags, opened;
static uint64_t start_ms;
static int press_x = -1, press_y = -1;

static const uint32_t num_colors[9] = { 0, 0xFF3B82F6, 0xFF16A34A, 0xFFEF4444, 0xFF7C3AED, 0xFFB45309, 0xFF0891B2, 0xFF111827, 0xFF6B7280 };

static void update_labels(void) {
    char t[32];
    snprintf(t, sizeof(t), "Mines: %d", nmines - flags);
    ui_set_text(mines_lbl, t);
    int secs = started ? (int)((uptime_ms() - start_ms) / 1000) : 0;
    if (over) secs = (int)(time_lbl->ival);
    snprintf(t, sizeof(t), "Time: %d", MIN(secs, 999));
    ui_set_text(time_lbl, t);
    ui_set_text(face_btn, won ? "You win!" : over ? "Try again" : "New game");
}

static void new_game(void) {
    memset(mine, 0, sizeof(mine));
    memset(state, 0, sizeof(state));
    started = over = won = false;
    flags = opened = 0;
    time_lbl->ival = 0;
    update_labels();
    ui_widget_invalidate(board);
}

static void place_mines(int sx, int sy) {
    srand((unsigned)uptime_ms());
    int placed = 0;
    while (placed < nmines) {
        int x = rand() % bw, y = rand() % bh;
        if (mine[y][x] || (abs(x - sx) <= 1 && abs(y - sy) <= 1)) continue;
        mine[y][x] = 1;
        placed++;
    }
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < bw && ny < bh && mine[ny][nx]) n++;
                }
            adj[y][x] = n;
        }
}

static void reveal(int x, int y) {
    if (x < 0 || y < 0 || x >= bw || y >= bh || state[y][x] != 0) return;
    state[y][x] = 1;
    opened++;
    if (mine[y][x]) {
        over = true;
        time_lbl->ival = (int)((uptime_ms() - start_ms) / 1000);
        for (int yy = 0; yy < bh; yy++)
            for (int xx = 0; xx < bw; xx++)
                if (mine[yy][xx] && state[yy][xx] == 0) state[yy][xx] = 1;
        state[y][x] = 3;   /* the exploded one */
        return;
    }
    if (adj[y][x] == 0)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) reveal(x + dx, y + dy);
}

static void check_win(void) {
    if (!over && opened == bw * bh - nmines) {
        over = won = true;
        time_lbl->ival = (int)((uptime_ms() - start_ms) / 1000);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
                if (mine[y][x]) state[y][x] = 2;
        flags = nmines;
    }
}

static void draw(ui_widget_t *w, surface_t *s) {
    bool dark = ui_theme.dark;
    uint32_t hidden = dark ? 0xFF3A3D45 : 0xFFC7CCD6, hidden_hi = dark ? 0xFF4A4E58 : 0xFFDCE0E8;
    uint32_t open_c = dark ? 0xFF24262B : 0xFFF1F2F5;
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int px = w->r.x + x * CELL, py = w->r.y + y * CELL;
            int st = state[y][x];
            if (st == 0 || st == 2) {
                bool pressed = x == press_x && y == press_y && st == 0;
                gfx_fill_rounded(s, px + 1, py + 1, CELL - 2, CELL - 2, 4, pressed ? open_c : hidden);
                if (!pressed) gfx_hline(s, px + 4, py + 2, CELL - 8, hidden_hi);
                if (st == 2) ui_draw_glyph(s, "flag", 16, px + 6, py + 6, 0xFFEF4444);
            } else {
                gfx_fill(s, px, py, CELL, CELL, st == 3 ? 0xFFEF4444 : open_c);
                gfx_rect(s, px, py, CELL, CELL, dark ? 0xFF1B1C20 : 0xFFDDE0E6);
                if (mine[y][x]) {
                    gfx_fill_circle(s, px + CELL / 2, py + CELL / 2, 7, 0xFF111111);
                    gfx_fill_circle(s, px + CELL / 2 - 2, py + CELL / 2 - 2, 2, 0xFFFFFFFF);
                } else if (adj[y][x]) {
                    char n[2] = { (char)('0' + adj[y][x]), 0 };
                    uint32_t c = num_colors[(int)adj[y][x]];
                    if (dark && adj[y][x] == 7) c = 0xFFE5E7EB;
                    ui_draw_text_center(s, &ui_font_bold, mkrect(px, py, CELL, CELL), n, c);
                }
            }
        }
    }
}

static bool event(ui_widget_t *w, gui_event_t *ev) {
    int x = (ev->x - w->r.x) / CELL, y = (ev->y - w->r.y) / CELL;
    bool inside = ev->x >= w->r.x && ev->y >= w->r.y && x < bw && y < bh;
    if (over) return true;
    if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_LEFT && inside) {
        press_x = x;
        press_y = y;
        ui_widget_invalidate(w);
    } else if (ev->type == EV_MOUSE_MOVE && (ev->buttons & MOUSE_LEFT)) {
        press_x = inside ? x : -1;
        press_y = inside ? y : -1;
        ui_widget_invalidate(w);
    } else if (ev->type == EV_MOUSE_UP && ev->button == MOUSE_LEFT) {
        press_x = press_y = -1;
        if (inside && state[y][x] == 0) {
            if (!started) { place_mines(x, y); started = true; start_ms = uptime_ms(); }
            reveal(x, y);
            check_win();
        } else if (inside && state[y][x] == 1 && adj[y][x]) {
            /* chord: open neighbours if enough flags are set */
            int f = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < bw && ny < bh && state[ny][nx] == 2) f++;
                }
            if (f == adj[y][x])
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) reveal(x + dx, y + dy);
            check_win();
        }
        update_labels();
        ui_widget_invalidate(w);
    } else if (ev->type == EV_MOUSE_DOWN && ev->button == MOUSE_RIGHT && inside) {
        if (state[y][x] == 0) { state[y][x] = 2; flags++; }
        else if (state[y][x] == 2) { state[y][x] = 0; flags--; }
        update_labels();
        ui_widget_invalidate(w);
    }
    return true;
}

static void set_level(int w, int h, int m) {
    bw = w;
    bh = h;
    nmines = m;
    int W = MAX(bw * CELL + 24, 300), H = TOP + bh * CELL + 12;
    board->r = mkrect((W - bw * CELL) / 2, TOP, bw * CELL, bh * CELL);
    ui_resize(win, W, H);
    new_game();
}

static void on_face(ui_widget_t *w) { (void)w; new_game(); }

enum { M_NEW = 1, M_BEGINNER, M_INTERMEDIATE, M_EXPERT, M_EXIT };
static void menu_cb(ui_window_t *w, int id) {
    (void)w;
    if (id == M_NEW) new_game();
    else if (id == M_BEGINNER) set_level(9, 9, 10);
    else if (id == M_INTERMEDIATE) set_level(16, 16, 40);
    else if (id == M_EXPERT) set_level(30, 16, 99);
    else if (id == M_EXIT) { ui_window_close(win); ui_quit(); }
}

static void on_resize(ui_window_t *w) {
    board->r = mkrect((w->w - bw * CELL) / 2, TOP, bw * CELL, bh * CELL);
    time_lbl->r.x = w->w - 110;
    face_btn->r.x = (w->w - 110) / 2;
    ui_invalidate(w);
}

static void tick(void *arg) {
    (void)arg;
    if (started && !over) update_labels();
}

int main(void) {
    int W = 9 * CELL + 24 < 300 ? 300 : 9 * CELL + 24, H = TOP + 9 * CELL + 12;
    win = ui_window("Minesweeper", W, H, 0, "mines");
    if (!win) return 1;
    win->on_resize = on_resize;
    ui_widget_t *bar = ui_menubar(win, menu_cb);
    ui_menu_t *m = ui_menu_new();
    ui_menu_item(m, "New Game", "F2", M_NEW);
    ui_menu_separator(m);
    ui_menu_item(m, "Beginner (9x9)", 0, M_BEGINNER);
    ui_menu_item(m, "Intermediate (16x16)", 0, M_INTERMEDIATE);
    ui_menu_item(m, "Expert (30x16)", 0, M_EXPERT);
    ui_menu_separator(m);
    ui_menu_item(m, "Exit", 0, M_EXIT);
    ui_menubar_add(bar, "Game", m);
    mines_lbl = ui_label(win, 12, 32, 100, 28, "");
    time_lbl = ui_label(win, W - 110, 32, 100, 28, "");
    time_lbl->align = 2;
    face_btn = ui_button(win, (W - 110) / 2, 32, 110, 28, "New game", on_face);
    face_btn->focusable = false;
    board = ui_canvas(win, (W - 9 * CELL) / 2, TOP, 9 * CELL, 9 * CELL, draw, event);
    board->focusable = false;
    new_game();
    ui_timer(500, tick, 0);
    ui_run();
    return 0;
}
