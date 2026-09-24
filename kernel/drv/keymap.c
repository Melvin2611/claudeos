/* Keyboard layouts: German (QWERTZ, default) and US (QWERTY) */
#include <kernel.h>
#include <input.h>

#define DEAD 0x80000000u   /* marks a dead key */
#define N 0x59

typedef struct {
    const char *name;
    uint32_t normal[N], shift[N], altgr[N];
} keymap_t;

/* helper macro for the common part (control keys) */
static const keymap_t km_us = {
    "us",
    {
        [0x01] = 27, [0x02] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
        [0x10] = 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        [0x1E] = 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
        [0x2B] = '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
        [0x37] = '*', [0x39] = ' ', [0x4A] = '-', [0x4E] = '+', [0x56] = '\\',
    },
    {
        [0x01] = 27, [0x02] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
        [0x10] = 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        [0x1E] = 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
        [0x2B] = '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
        [0x37] = '*', [0x39] = ' ', [0x4A] = '-', [0x4E] = '+', [0x56] = '|',
    },
    { [0] = 0 },
};

static const keymap_t km_de = {
    "de",
    {
        [0x01] = 27, [0x02] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xDF, DEAD | 0xB4, '\b', '\t',
        [0x10] = 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 0xFC, '+', '\n',
        [0x1E] = 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0xF6, 0xE4, DEAD | '^',
        [0x2B] = '#', 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-',
        [0x37] = '*', [0x39] = ' ', [0x4A] = '-', [0x4E] = '+', [0x56] = '<',
    },
    {
        [0x01] = 27, [0x02] = '!', '"', 0xA7, '$', '%', '&', '/', '(', ')', '=', '?', DEAD | '`', '\b', '\t',
        [0x10] = 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 0xDC, '*', '\n',
        [0x1E] = 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xD6, 0xC4, 0xB0,
        [0x2B] = '\'', 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_',
        [0x37] = '*', [0x39] = ' ', [0x4A] = '-', [0x4E] = '+', [0x56] = '>',
    },
    {
        [0x03] = 0xB2, [0x04] = 0xB3, [0x08] = '{', [0x09] = '[', [0x0A] = ']', [0x0B] = '}', [0x0C] = '\\',
        [0x10] = '@', [0x12] = 0x20AC, [0x1B] = '~', [0x32] = 0xB5, [0x56] = '|', [0x39] = ' ',
    },
};

static const keymap_t *maps[] = { &km_de, &km_us };
static const keymap_t *active = &km_de;

bool keymap_select(const char *name) {
    for (size_t i = 0; i < ARRAY_SIZE(maps); i++) {
        if (!strcmp(maps[i]->name, name)) { active = maps[i]; return true; }
    }
    return false;
}

const char *keymap_name(void) { return active->name; }

static bool is_letter(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xFE && c != 0xD7 && c != 0xF7 && c != 0xDF);
}

static uint32_t flip_case(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0xE0 && c <= 0xFE) return c - 32;
    if (c >= 0xC0 && c <= 0xDE) return c + 32;
    return c;
}

uint32_t keymap_translate(int keycode, uint32_t mods, bool *dead) {
    *dead = false;
    /* keypad */
    if (keycode >= 0x47 && keycode <= 0x53 && keycode != 0x4A && keycode != 0x4E) {
        if (!(mods & MOD_NUM)) return 0;
        static const char kp[] = "789-456+1230.";
        char c = kp[keycode - 0x47];
        if (c == '.' && active == &km_de) c = ',';
        return (uint32_t)c;
    }
    if (keycode == KEY_KPENTER) return '\n';
    if (keycode == KEY_KPSLASH) return '/';
    if (keycode >= N) return 0;
    uint32_t c;
    if ((mods & MOD_ALTGR) || ((mods & MOD_CTRL) && (mods & MOD_ALT))) {
        c = active->altgr[keycode];
        if (!c) return 0;
    } else if (mods & MOD_SHIFT) {
        c = active->shift[keycode];
    } else {
        c = active->normal[keycode];
    }
    if (c & DEAD) {
        *dead = true;
        return c & ~DEAD;
    }
    if ((mods & MOD_CAPS) && is_letter(c)) c = flip_case(c);
    return c;
}

uint32_t keymap_compose(uint32_t dead, uint32_t ch) {
    static const uint32_t circ[] = { 0xE2, 0xEA, 0xEE, 0xF4, 0xFB, 0xC2, 0xCA, 0xCE, 0xD4, 0xDB };
    static const uint32_t acute[] = { 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xFD, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xDD };
    static const uint32_t grave[] = { 0xE0, 0xE8, 0xEC, 0xF2, 0xF9, 0xC0, 0xC8, 0xCC, 0xD2, 0xD9 };
    static const struct { uint32_t dead; const char *base; const uint32_t *out; } tbl[] = {
        { '^', "aeiouAEIOU", circ },
        { 0xB4, "aeiouyAEIOUY", acute },
        { '`', "aeiouAEIOU", grave },
    };
    if (ch == ' ') return dead == 0xB4 ? 0xB4 : dead;
    for (size_t i = 0; i < ARRAY_SIZE(tbl); i++) {
        if (tbl[i].dead != dead) continue;
        for (int j = 0; tbl[i].base[j]; j++)
            if ((uint32_t)tbl[i].base[j] == ch) return tbl[i].out[j];
    }
    return 0;
}
