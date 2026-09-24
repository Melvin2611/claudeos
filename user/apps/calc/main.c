/* Calculator */
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static ui_window_t *win;
static char entry[40] = "0";      /* number being typed / shown */
static char expr[80] = "";        /* pending expression shown above */
static double acc;
static int pending;               /* 0 none, '+', '-', '*', '/' */
static bool fresh = true;         /* next digit starts a new number */
static bool error;
static double memory;

static void format_num(double v, char *out, size_t n) {
    if (isnan(v) || isinf(v)) { strlcpy(out, "Error", n); return; }
    if (fabs(v) < 1e-15) v = 0;
    char buf[64];
    if (fabs(v) >= 1e15 || (fabs(v) < 1e-6 && v != 0)) snprintf(buf, sizeof(buf), "%.10e", v);
    else snprintf(buf, sizeof(buf), "%.12f", v);
    /* strip trailing zeros in fixed notation */
    if (!strchr(buf, 'e') && strchr(buf, '.')) {
        int l = (int)strlen(buf);
        while (l > 0 && buf[l - 1] == '0') buf[--l] = 0;
        if (l > 0 && buf[l - 1] == '.') buf[--l] = 0;
    }
    if (!strcmp(buf, "-0")) strcpy(buf, "0");
    strlcpy(out, buf, n);
}

static double cur(void) { return strtod(entry, 0); }

static const char *op_str(int op) {
    return op == '+' ? "+" : op == '-' ? "\xE2\x88\x92" : op == '*' ? "\xC3\x97" : op == '/' ? "\xC3\xB7" : "";
}

static double apply(double a, int op, double b) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': if (b == 0) { error = true; return NAN; } return a / b;
    }
    return b;
}

static void clear_all(void) {
    strcpy(entry, "0");
    expr[0] = 0;
    acc = 0;
    pending = 0;
    fresh = true;
    error = false;
}

static void set_result(double v) {
    format_num(v, entry, sizeof(entry));
    if (!strcmp(entry, "Error")) error = true;
    fresh = true;
}

static void digit(char c) {
    if (error) clear_all();
    if (fresh) {
        strcpy(entry, c == '.' ? "0." : (char[]){ c, 0 });
        fresh = false;
        return;
    }
    if (c == '.' && strchr(entry, '.')) return;
    if (strlen(entry) >= 18) return;
    if (!strcmp(entry, "0") && c != '.') { entry[0] = c; return; }
    if (!strcmp(entry, "-0") && c != '.') { entry[1] = c; return; }
    size_t l = strlen(entry);
    entry[l] = c;
    entry[l + 1] = 0;
}

static void op(int o) {
    if (error) return;
    if (pending && !fresh) acc = apply(acc, pending, cur());
    else if (!pending) acc = cur();
    pending = o;
    char a[40];
    format_num(acc, a, sizeof(a));
    snprintf(expr, sizeof(expr), "%s %s", a, op_str(o));
    set_result(acc);
}

static void equals(void) {
    if (error || !pending) return;
    double b = cur();
    char a[40], bs[40];
    format_num(acc, a, sizeof(a));
    format_num(b, bs, sizeof(bs));
    double r = apply(acc, pending, b);
    snprintf(expr, sizeof(expr), "%s %s %s =", a, op_str(pending), bs);
    pending = 0;
    acc = r;
    set_result(r);
}

static void unary(const char *what) {
    if (error) return;
    double v = cur(), r = v;
    char vs[40];
    format_num(v, vs, sizeof(vs));
    if (!strcmp(what, "sqrt")) { if (v < 0) error = true; r = sqrt(v); snprintf(expr, sizeof(expr), "\xE2\x88\x9A(%s)", vs); }
    else if (!strcmp(what, "sqr")) { r = v * v; snprintf(expr, sizeof(expr), "sqr(%s)", vs); }
    else if (!strcmp(what, "inv")) { if (v == 0) error = true; r = 1 / v; snprintf(expr, sizeof(expr), "1/(%s)", vs); }
    else if (!strcmp(what, "neg")) {
        if (entry[0] == '-') memmove(entry, entry + 1, strlen(entry));
        else if (strcmp(entry, "0")) { memmove(entry + 1, entry, strlen(entry) + 1); entry[0] = '-'; }
        return;
    } else if (!strcmp(what, "pct")) { r = pending ? acc * v / 100 : v / 100; }
    set_result(error ? NAN : r);
    if (error) strcpy(entry, "Error");
}

static void backspace(void) {
    if (error) { clear_all(); return; }
    if (fresh) return;
    size_t l = strlen(entry);
    if (l <= 1 || (l == 2 && entry[0] == '-')) { strcpy(entry, "0"); fresh = true; return; }
    entry[l - 1] = 0;
}

static void press(const char *k) {
    if (!k[1] && ((k[0] >= '0' && k[0] <= '9') || k[0] == '.')) digit(k[0]);
    else if (!strcmp(k, "+")) op('+');
    else if (!strcmp(k, "-")) op('-');
    else if (!strcmp(k, "*")) op('*');
    else if (!strcmp(k, "/")) op('/');
    else if (!strcmp(k, "=")) equals();
    else if (!strcmp(k, "C")) clear_all();
    else if (!strcmp(k, "CE")) { strcpy(entry, "0"); fresh = true; error = false; }
    else if (!strcmp(k, "BS")) backspace();
    else if (!strcmp(k, "MC")) memory = 0;
    else if (!strcmp(k, "MR")) set_result(memory);
    else if (!strcmp(k, "M+")) memory += cur();
    else if (!strcmp(k, "M-")) memory -= cur();
    else unary(k);
    ui_invalidate_rect(win, mkrect(0, 0, win->w, 110));
}

typedef struct { const char *label; const char *key; int kind; } key_t_;
static const key_t_ keys[] = {
    { "MC", "MC", 2 }, { "MR", "MR", 2 }, { "M+", "M+", 2 }, { "M\xE2\x88\x92", "M-", 2 },
    { "%", "pct", 1 }, { "CE", "CE", 1 }, { "C", "C", 1 }, { "\xE2\x86\x90", "BS", 1 },
    { "1/x", "inv", 1 }, { "x\xC2\xB2", "sqr", 1 }, { "\xE2\x88\x9Ax", "sqrt", 1 }, { "\xC3\xB7", "/", 1 },
    { "7", "7", 0 }, { "8", "8", 0 }, { "9", "9", 0 }, { "\xC3\x97", "*", 1 },
    { "4", "4", 0 }, { "5", "5", 0 }, { "6", "6", 0 }, { "\xE2\x88\x92", "-", 1 },
    { "1", "1", 0 }, { "2", "2", 0 }, { "3", "3", 0 }, { "+", "+", 1 },
    { "\xC2\xB1", "neg", 0 }, { "0", "0", 0 }, { ".", ".", 0 }, { "=", "=", 3 },
};

static void on_button(ui_widget_t *w) { press(keys[w->id].key); }

static void paint(ui_window_t *w, surface_t *s) {
    int dw = w->w - 24;
    font_draw_fit(s, &ui_font_large, 12, 16, expr, dw, ui_theme.window_text_dim);
    const font_t *f = &ui_font_display;
    int tw = font_text_width(f, entry);
    if (tw > dw) { f = &ui_font_big; tw = font_text_width(f, entry); }
    if (tw > dw) { f = &ui_font_title; tw = font_text_width(f, entry); }
    font_draw(s, f, w->w - 12 - tw, 44 + (48 - f->height) / 2, entry, ui_theme.window_text);
    if (memory != 0) font_draw(s, &ui_font_bold, 12, 88, "M", ui_theme.accent);
}

static void on_key(ui_window_t *w, gui_event_t *ev) {
    (void)w;
    uint32_t c = ev->ch;
    if (ev->mods & MOD_CTRL) {
        if (c == 'c' || c == 'C') gui_clipboard_set(entry, strlen(entry));
        if (c == 'v' || c == 'V') {
            char buf[40];
            long n = gui_clipboard_get(buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[MIN(n, 39)] = 0;
                double v = strtod(buf, 0);
                set_result(v);
                fresh = false;
                ui_invalidate(win);
            }
        }
        return;
    }
    if ((c >= '0' && c <= '9') || c == '.' || c == ',') { char k[2] = { c == ',' ? '.' : (char)c, 0 }; press(k); return; }
    switch (c) {
    case '+': press("+"); return;
    case '-': press("-"); return;
    case '*': press("*"); return;
    case '/': press("/"); return;
    case '=': case '\n': press("="); return;
    case '%': press("pct"); return;
    }
    if (ev->key == KEY_BACKSPACE) press("BS");
    else if (ev->key == KEY_ESC) press("C");
    else if (ev->key == KEY_DELETE) press("CE");
    else if (ev->key == KEY_ENTER || ev->key == KEY_KPENTER) press("=");
}

int main(void) {
    int bw = 76, bh = 52, gap = 4;
    int W = 4 * bw + 5 * gap + 8, H = 112 + 7 * (bh + gap) + 8;
    win = ui_window("Calculator", W, H, 0, "calculator");
    if (!win) return 1;
    win->on_paint = paint;
    win->on_key = on_key;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        int r = (int)i / 4, c = (int)i % 4;
        int x = 6 + gap + c * (bw + gap), y = 112 + r * (bh + gap);
        int h = r == 0 ? bh - 16 : bh;
        if (r > 0) y -= 16;
        ui_widget_t *b = ui_button(win, x, y, bw, h, keys[i].label, on_button);
        b->id = (int)i;
        b->focusable = false;
        b->font = keys[i].kind == 0 ? &ui_font_large : keys[i].kind == 2 ? &ui_font : &ui_font_large;
        if (keys[i].kind == 3) ui_button_style(b, BTN_PRIMARY);
    }
    ui_run();
    return 0;
}
