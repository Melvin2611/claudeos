#pragma once
#include <claudeos/gfx.h>

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t version, count;
    int16_t size, ascent, descent, line_height;
    uint16_t flags, reserved;
    uint32_t bitmap_offset;
} fnt_header_t;

typedef struct __attribute__((packed)) {
    uint32_t cp;
    int16_t bx, by;
    uint16_t w, h;
    uint16_t adv;      /* 1/64 px */
    uint16_t pad;
    uint32_t off;
} fnt_glyph_t;

typedef struct font {
    const uint8_t *data;
    const fnt_header_t *hdr;
    const fnt_glyph_t *glyphs;
    const uint8_t *bitmap;
    const fnt_glyph_t *ascii[128];
    const fnt_glyph_t *fallback;
    int ascent, descent, height, cell;   /* cell: monospace advance in px (0 if proportional) */
} font_t;

bool font_load(font_t *f, const void *data, size_t size);
const fnt_glyph_t *font_glyph(const font_t *f, uint32_t cp);
int font_text_width(const font_t *f, const char *s);
int font_text_width_n(const font_t *f, const char *s, int nbytes);
/* draw text with the top of the line box at y; returns x after the text */
int font_draw(surface_t *s, const font_t *f, int x, int y, const char *text, uint32_t color);
int font_draw_n(surface_t *s, const font_t *f, int x, int y, const char *text, int nbytes, uint32_t color);
int font_draw_cp(surface_t *s, const font_t *f, int x, int y, uint32_t cp, uint32_t color);
/* draw text shortened with "…" to fit max_w */
void font_draw_fit(surface_t *s, const font_t *f, int x, int y, const char *text, int max_w, uint32_t color);
/* byte index of the character at pixel offset px (for mouse hit-testing) */
int font_index_at(const font_t *f, const char *s, int px);

/* ---- UTF-8 ---- */
uint32_t utf8_decode(const char **s);                   /* advances *s; returns 0xFFFD on error */
int utf8_encode(uint32_t cp, char *out);                /* returns bytes written (1-4) */
int utf8_next(const char *s, int i);                    /* index of next char */
int utf8_prev(const char *s, int i);                    /* index of previous char */
int utf8_len(const char *s);                            /* number of code points */
