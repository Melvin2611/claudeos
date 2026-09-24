/* Tetris */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <claudeos.h>

#define COLS 10
#define ROWS 20
#define CELL 28
#define SIDE 170

static ui_window_t *win;
static ui_widget_t *field;
static uint8_t grid[ROWS][COLS];      /* 0 empty, 1..7 piece colour */
static int piece, rot, px, py, next_piece;
static int score, lines_cleared, level, best;
static bool running, over, paused;
static int timer_id = -1;
static int bag[7], bag_pos = 7;

static const uint32_t colors[8] = { 0, 0xFF22D3EE, 0xFFFACC15, 0xFFA855F7, 0xFF22C55E, 0xFFEF4444, 0xFF3B82F6, 0xFFF97316 };

/* 7 tetrominoes x 4 rotations, 4 cells each as (x,y) in a 4x4 box */
static const int8_t shapes[7][4][8] = {
    { {0,1,1,1,2,1,3,1}, {2,0,2,1,2,2,2,3}, {0,2,1,2,2,2,3,2}, {1,0,1,1,1,2,1,3} },   /* I */
    { {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1}, {1,0,2,0,1,1,2,1} },   /* O */
    { {1,0,0,1,1,1,2,1}, {1,0,1,1,2,1,1,2}, {0,1,1,1,2,1,1,2}, {1,0,0,1,1,1,1,2} },   /* T */
    { {1,0,2,0,0,1,1,1}, {1,0,1,1,2,1,2,2}, {1,1,2,1,0,2,1,2}, {0,0,0,1,1,1,1,2} },   /* S */
    { {0,0,1,0,1,1,2,1}, {2,0,1,1,2,1,1,2}, {0,1,1,1,1,2,2,2}, {1,0,0,1,1,1,0,2} },   /* Z */
    { {0,0,0,1,1,1,2,1}, {1,0,2,0,1,1,1,2}, {0,1,1,1,2,1,2,2}, {1,0,1,1,0,2,1,2} },   /* J */
    { {2,0,0,1,1,1,2,1}, {1,0,1,1,1,2,2,2}, {0,1,1,1,2,1,0,2}, {0,0,1,0,1,1,1,2} },   /* L */
};

static int draw_bag(void) {
    if (bag_pos >= 7) {
        for (int i = 0; i < 7; i++) bag[i] = i;
        for (int i = 6; i > 0; i--) { int j = rand() % (i + 1); int t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
        bag_pos = 0;
    }
    return bag[bag_pos++];
}

static bool fits(int p, int r, int x, int y) {
    for (int i = 0; i < 4; i++) {
        int cx = x + shapes[p][r][i * 2], cy = y + shapes[p][r][i * 2 + 1];
        if (cx < 0 || cx >= COLS || cy >= ROWS) return false;
        if (cy >= 0 && grid[cy][cx]) return false;
    }
    return true;
}

static void tick(void *arg);

static void set_speed(void) {
    if (timer_id >= 0) ui_timer_cancel(timer_id);
    int ms = MAX(80, 700 - level * 60);
    timer_id = ui_timer(ms, tick, 0);
}

static void spawn_piece(void) {
    piece = next_piece;
    next_piece = draw_bag();
    rot = 0;
    px = 3;
    py = -1;
    if (!fits(piece, rot, px, py + 1) && !fits(piece, rot, px, py)) {
        over = true;
        if (score > best) best = score;
        beep(196, 300);
    }
}

static void new_game(void) {
    memset(grid, 0, sizeof(grid));
    score = lines_cleared = 0;
    level = 1;
    over = paused = false;
    running = true;
    srand((unsigned)uptime_ms());
    bag_pos = 7;
    next_piece = draw_bag();
    spawn_piece();
    set_speed();
    ui_invalidate(win);
}

static void lock_piece(void) {
    for (int i = 0; i < 4; i++) {
        int cx = px + shapes[piece][rot][i * 2], cy = py + shapes[piece][rot][i * 2 + 1];
        if (cy >= 0) grid[cy][cx] = (uint8_t)(piece + 1);
    }
    int cleared = 0;
    for (int y = ROWS - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < COLS; x++) if (!grid[y][x]) full = false;
        if (full) {
            memmove(&grid[1], &grid[0], sizeof(grid[0]) * y);
            memset(grid[0], 0, sizeof(grid[0]));
            cleared++;
            y++;
        }
    }
    static const int pts[5] = { 0, 100, 300, 500, 800 };
    score += pts[cleared] * level;
    lines_cleared += cleared;
    if (lines_cleared / 10 + 1 > level) { level = lines_cleared / 10 + 1; set_speed(); }
    if (cleared) beep(660 + cleared * 110, 60);
    spawn_piece();
}

static void tick(void *arg) {
    (void)arg;
    if (!running || over || paused) return;
    if (fits(piece, rot, px, py + 1)) py++;
    else lock_piece();
    ui_invalidate(win);
}

static void cell(surface_t *s, int x, int y, uint32_t c, int size) {
    gfx_fill_rounded(s, x + 1, y + 1, size - 2, size - 2, 4, c);
    gfx_hline(s, x + 4, y + 3, size - 8, gfx_lighten(c, 90));
    gfx_hline(s, x + 4, y + size - 4, size - 8, gfx_darken(c, 60));
}

static void draw(ui_widget_t *w, surface_t *s) {
    int ox = w->r.x, oy = w->r.y;
    gfx_fill_rounded(s, ox - 4, oy - 4, COLS * CELL + 8, ROWS * CELL + 8, 8, 0xFF15171C);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++) {
            gfx_rect(s, ox + x * CELL, oy + y * CELL, CELL, CELL, 0xFF1D2027);
            if (grid[y][x]) cell(s, ox + x * CELL, oy + y * CELL, colors[grid[y][x]], CELL);
        }
    if (running && !over) {
        /* ghost */
        int gy = py;
        while (fits(piece, rot, px, gy + 1)) gy++;
        for (int i = 0; i < 4; i++) {
            int cx = px + shapes[piece][rot][i * 2], cy = gy + shapes[piece][rot][i * 2 + 1];
            if (cy >= 0) gfx_rounded_rect(s, ox + cx * CELL + 2, oy + cy * CELL + 2, CELL - 4, CELL - 4, 4, WITH_ALPHA(colors[piece + 1], 120));
        }
        for (int i = 0; i < 4; i++) {
            int cx = px + shapes[piece][rot][i * 2], cy = py + shapes[piece][rot][i * 2 + 1];
            if (cy >= 0) cell(s, ox + cx * CELL, oy + cy * CELL, colors[piece + 1], CELL);
        }
    }
    if (!running || over || paused) {
        gfx_fill_rounded(s, ox + 10, oy + ROWS * CELL / 2 - 50, COLS * CELL - 20, 100, 12, 0xD8000000);
        const char *t1 = over ? "Game over" : paused ? "Paused" : "Tetris";
        const char *t2 = over || !running ? "Press Enter to start" : "Press P to continue";
        ui_draw_text_center(s, &ui_font_title, mkrect(ox, oy + ROWS * CELL / 2 - 40, COLS * CELL, 30), t1, 0xFFFFFFFF);
        ui_draw_text_center(s, &ui_font, mkrect(ox, oy + ROWS * CELL / 2, COLS * CELL, 24), t2, 0xFFD1D5DB);
    }
}

static void paint(ui_window_t *w, surface_t *s) {
    int x = 20 + COLS * CELL + 24;
    font_draw(s, &ui_font_bold, x, 20, "NEXT", ui_theme.window_text_dim);
    gfx_fill_rounded(s, x, 44, 128, 96, 8, ui_theme.input_bg);
    if (running) {
        for (int i = 0; i < 4; i++) {
            int cx = shapes[next_piece][0][i * 2], cy = shapes[next_piece][0][i * 2 + 1];
            cell(s, x + 20 + cx * 22, 44 + 26 + cy * 22, colors[next_piece + 1], 22);
        }
    }
    char t[32];
    const char *labels[] = { "SCORE", "LINES", "LEVEL", "BEST" };
    int vals[] = { score, lines_cleared, level, best };
    for (int i = 0; i < 4; i++) {
        font_draw(s, &ui_font_bold, x, 160 + i * 60, labels[i], ui_theme.window_text_dim);
        snprintf(t, sizeof(t), "%d", vals[i]);
        font_draw(s, &ui_font_title, x, 180 + i * 60, t, ui_theme.window_text);
    }
    font_draw(s, &ui_font, x, w->h - 70, "\xE2\x86\x90 \xE2\x86\x92  move", ui_theme.window_text_dim);
    font_draw(s, &ui_font, x, w->h - 52, "\xE2\x86\x91  rotate", ui_theme.window_text_dim);
    font_draw(s, &ui_font, x, w->h - 34, "Space  drop, P pause", ui_theme.window_text_dim);
}

static void key(gui_event_t *ev) {
    int k = ev->key;
    if (!running || over) {
        if (k == KEY_ENTER || k == KEY_SPACE) new_game();
        return;
    }
    if (ev->ch == 'p' || k == KEY_ESC) { paused = !paused; ui_invalidate(win); return; }
    if (paused) return;
    if (k == KEY_LEFT && fits(piece, rot, px - 1, py)) px--;
    else if (k == KEY_RIGHT && fits(piece, rot, px + 1, py)) px++;
    else if (k == KEY_DOWN) { if (fits(piece, rot, px, py + 1)) { py++; score += 1; } }
    else if (k == KEY_UP || ev->ch == 'x') {
        int nr = (rot + 1) % 4;
        static const int kicks[] = { 0, -1, 1, -2, 2 };
        for (int i = 0; i < 5; i++)
            if (fits(piece, nr, px + kicks[i], py)) { rot = nr; px += kicks[i]; break; }
    } else if (k == KEY_SPACE) {
        while (fits(piece, rot, px, py + 1)) { py++; score += 2; }
        lock_piece();
    }
    ui_invalidate(win);
}

static bool field_event(ui_widget_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->type == EV_KEY_DOWN) key(ev);
    return true;
}

static void on_event(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->type == EV_UNFOCUS && running && !over) { paused = true; ui_invalidate(win); }
}

int main(void) {
    int W = 20 + COLS * CELL + 24 + 150, H = 20 + ROWS * CELL + 20;
    win = ui_window("Tetris", W, H, 0, "tetris");
    if (!win) return 1;
    win->on_paint = paint;
    win->on_event = on_event;
    field = ui_canvas(win, 20, 20, COLS * CELL, ROWS * CELL, draw, field_event);
    ui_focus(win, field);
    next_piece = 0;
    ui_run();
    return 0;
}
