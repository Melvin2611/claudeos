/* Snake */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <claudeos.h>

#define GW 28
#define GH 20
#define CELL 22
#define HUD 40

static ui_window_t *win;
static ui_widget_t *field;
static int sx[GW * GH], sy[GW * GH], len;
static int dir_x = 1, dir_y, next_dx = 1, next_dy;
static int food_x, food_y;
static int score, best;
static bool running, dead, paused;
static int timer_id = -1;

static void place_food(void) {
    for (;;) {
        food_x = rand() % GW;
        food_y = rand() % GH;
        bool hit = false;
        for (int i = 0; i < len; i++) if (sx[i] == food_x && sy[i] == food_y) hit = true;
        if (!hit) return;
    }
}

static void step(void *arg);

static void set_speed(void) {
    if (timer_id >= 0) ui_timer_cancel(timer_id);
    int ms = MAX(55, 140 - score / 5 * 6);
    timer_id = ui_timer(ms, step, 0);
}

static void reset(void) {
    len = 4;
    for (int i = 0; i < len; i++) { sx[i] = GW / 2 - i; sy[i] = GH / 2; }
    dir_x = next_dx = 1;
    dir_y = next_dy = 0;
    score = 0;
    dead = false;
    paused = false;
    running = true;
    srand((unsigned)uptime_ms());
    place_food();
    set_speed();
    ui_invalidate(win);
}

static void step(void *arg) {
    (void)arg;
    if (!running || dead || paused) return;
    dir_x = next_dx;
    dir_y = next_dy;
    int nx = sx[0] + dir_x, ny = sy[0] + dir_y;
    if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) { dead = true; }
    for (int i = 0; i < len - 1 && !dead; i++) if (sx[i] == nx && sy[i] == ny) dead = true;
    if (dead) {
        if (score > best) best = score;
        beep(220, 120);
        ui_invalidate(win);
        return;
    }
    bool grow = nx == food_x && ny == food_y;
    if (grow) len++;
    for (int i = len - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
    sx[0] = nx;
    sy[0] = ny;
    if (grow) {
        score += 10;
        place_food();
        if (score % 50 == 0) set_speed();
    }
    ui_invalidate(win);
}

static void draw(ui_widget_t *w, surface_t *s) {
    int ox = w->r.x, oy = w->r.y;
    gfx_fill_rounded(s, ox, oy, GW * CELL, GH * CELL, 10, 0xFF14231B);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            if ((x + y) & 1) gfx_fill(s, ox + x * CELL, oy + y * CELL, CELL, CELL, 0x0CFFFFFF);
    /* food */
    gfx_fill_circle(s, ox + food_x * CELL + CELL / 2, oy + food_y * CELL + CELL / 2 + 1, CELL / 2 - 3, 0xFFEF4444);
    gfx_line(s, ox + food_x * CELL + CELL / 2, oy + food_y * CELL + 3, ox + food_x * CELL + CELL / 2 + 3, oy + food_y * CELL, 0xFF7C4A1E);
    /* snake */
    for (int i = len - 1; i >= 0; i--) {
        int t = len > 1 ? i * 120 / (len - 1) : 0;
        uint32_t c = gfx_mix(0xFF4ADE80, 0xFF15803D, t);
        gfx_fill_rounded(s, ox + sx[i] * CELL + 1, oy + sy[i] * CELL + 1, CELL - 2, CELL - 2, 6, c);
    }
    /* eyes */
    int hx = ox + sx[0] * CELL + CELL / 2, hy = oy + sy[0] * CELL + CELL / 2;
    int ex = dir_y ? 4 : 0, ey = dir_x ? 4 : 0;
    gfx_fill_circle(s, hx + dir_x * 4 - ex, hy + dir_y * 4 - ey, 2, 0xFF0B1A10);
    gfx_fill_circle(s, hx + dir_x * 4 + ex, hy + dir_y * 4 + ey, 2, 0xFF0B1A10);
    if (dead || !running || paused) {
        gfx_fill_rounded(s, ox + GW * CELL / 2 - 150, oy + GH * CELL / 2 - 50, 300, 100, 12, 0xD0000000);
        const char *t1 = dead ? "Game over" : paused ? "Paused" : "Snake";
        const char *t2 = dead ? "Press Enter to play again" : paused ? "Press P to continue" : "Press Enter to start";
        ui_draw_text_center(s, &ui_font_title, mkrect(ox, oy + GH * CELL / 2 - 40, GW * CELL, 30), t1, 0xFFFFFFFF);
        ui_draw_text_center(s, &ui_font, mkrect(ox, oy + GH * CELL / 2, GW * CELL, 24), t2, 0xFFD1D5DB);
    }
}

static void paint(ui_window_t *w, surface_t *s) {
    char t[64];
    snprintf(t, sizeof(t), "Score: %d", score);
    font_draw(s, &ui_font_large, 14, 10, t, ui_theme.window_text);
    snprintf(t, sizeof(t), "Best: %d", best);
    int tw = font_text_width(&ui_font_large, t);
    font_draw(s, &ui_font_large, w->w - 14 - tw, 10, t, ui_theme.window_text_dim);
}

static void on_key(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    int k = ev->key;
    if ((k == KEY_UP || ev->ch == 'w') && dir_y == 0) { next_dx = 0; next_dy = -1; }
    else if ((k == KEY_DOWN || ev->ch == 's') && dir_y == 0) { next_dx = 0; next_dy = 1; }
    else if ((k == KEY_LEFT || ev->ch == 'a') && dir_x == 0) { next_dx = -1; next_dy = 0; }
    else if ((k == KEY_RIGHT || ev->ch == 'd') && dir_x == 0) { next_dx = 1; next_dy = 0; }
    else if (k == KEY_ENTER || k == KEY_SPACE) { if (dead || !running) reset(); }
    else if (ev->ch == 'p' || k == KEY_ESC) { if (running && !dead) { paused = !paused; ui_invalidate(win); } }
}

static bool field_event(ui_widget_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->type == EV_KEY_DOWN) { on_key(win, ev); return true; }
    if (ev->type == EV_MOUSE_DOWN && (dead || !running)) reset();
    return true;
}

static void on_event(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    if (ev->type == EV_UNFOCUS && running && !dead) { paused = true; ui_invalidate(win); }
}

int main(void) {
    int W = GW * CELL + 24, H = GH * CELL + HUD + 12;
    win = ui_window("Snake", W, H, 0, "snake");
    if (!win) return 1;
    win->on_paint = paint;
    win->on_key = on_key;
    win->on_event = on_event;
    field = ui_canvas(win, 12, HUD, GW * CELL, GH * CELL, draw, field_event);
    ui_focus(win, field);
    len = 4;
    for (int i = 0; i < len; i++) { sx[i] = GW / 2 - i; sy[i] = GH / 2; }
    food_x = GW / 2 + 5;
    food_y = GH / 2;
    ui_run();
    return 0;
}
