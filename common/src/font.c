/* Anti-aliased bitmap font rendering + UTF-8 helpers */
#include <claudeos/font.h>

size_t strlen(const char *s);

uint32_t utf8_decode(const char **ps) {
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = s[0];
    int n = 0;
    if (c < 0x80) { *ps += 1; return c; }
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *ps += 1; return 0xFFFD; }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *ps += i; return 0xFFFD; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *ps += n + 1;
    return c;
}

int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) {
        out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F);
    out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F);
    return 4;
}

int utf8_next(const char *s, int i) {
    if (!s[i]) return i;
    i++;
    while ((s[i] & 0xC0) == 0x80) i++;
    return i;
}

int utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
    return i;
}

int utf8_len(const char *s) {
    int n = 0;
    while (*s) { utf8_decode(&s); n++; }
    return n;
}

bool font_load(font_t *f, const void *data, size_t size) {
    const fnt_header_t *h = data;
    if (size < sizeof(fnt_header_t) || h->magic[0] != 'C' || h->magic[1] != 'F' || h->magic[2] != 'N' ||
        h->magic[3] != 'T')
        return false;
    f->data = data;
    f->hdr = h;
    f->glyphs = (const fnt_glyph_t *)((const uint8_t *)data + sizeof(fnt_header_t));
    f->bitmap = (const uint8_t *)data + h->bitmap_offset;
    f->ascent = h->ascent;
    f->descent = h->descent;
    f->height = h->line_height;
    for (int i = 0; i < 128; i++) f->ascii[i] = 0;
    f->fallback = 0;
    for (int i = 0; i < h->count; i++) {
        const fnt_glyph_t *g = &f->glyphs[i];
        if (g->cp < 128) f->ascii[g->cp] = g;
        if (g->cp == '?') f->fallback = g;
    }
    f->cell = (h->flags & 1) && f->ascii['M'] ? f->ascii['M']->adv / 64 : 0;
    return true;
}

const fnt_glyph_t *font_glyph(const font_t *f, uint32_t cp) {
    if (cp < 128) return f->ascii[cp] ? f->ascii[cp] : f->fallback;
    int lo = 0, hi = f->hdr->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t c = f->glyphs[mid].cp;
        if (c == cp) return &f->glyphs[mid];
        if (c < cp) lo = mid + 1; else hi = mid - 1;
    }
    /* a few sensible substitutions */
    if (cp == 0x00A0) return f->ascii[' '];
    return f->fallback;
}

int font_text_width_n(const font_t *f, const char *s, int nbytes) {
    int x64 = 0;
    const char *end = s + nbytes;
    while (s < end && *s) {
        uint32_t cp = utf8_decode(&s);
        const fnt_glyph_t *g = font_glyph(f, cp);
        if (cp == '\t') g = f->ascii[' '], x64 += g ? g->adv * 3 : 0;
        if (g) x64 += g->adv;
    }
    return (x64 + 32) >> 6;
}

int font_text_width(const font_t *f, const char *s) { return font_text_width_n(f, s, (int)strlen(s)); }

static void draw_glyph(surface_t *s, const font_t *f, const fnt_glyph_t *g, int x, int ybase, uint32_t color) {
    int gx = x + g->bx, gy = ybase - g->by;
    rect_t c = s->clip;
    int x0 = gx < c.x ? c.x - gx : 0;
    int y0 = gy < c.y ? c.y - gy : 0;
    int x1 = gx + g->w > c.x + c.w ? c.x + c.w - gx : g->w;
    int y1 = gy + g->h > c.y + c.h ? c.y + c.h - gy : g->h;
    if (x0 >= x1 || y0 >= y1) return;
    const uint8_t *bm = f->bitmap + g->off;
    uint32_t ca = COL_A(color);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = &s->px[(gy + j) * s->stride + gx];
        const uint8_t *src = &bm[j * g->w];
        for (int i = x0; i < x1; i++) {
            uint32_t a = src[i];
            if (!a) continue;
            if (ca != 255) a = (a * ca + 127) / 255;
            row[i] = a >= 255 ? (color | 0xFF000000u) : gfx_blend(row[i], color, a);
        }
    }
}

int font_draw_n(surface_t *s, const font_t *f, int x, int y, const char *text, int nbytes, uint32_t color) {
    int x64 = x << 6;
    int ybase = y + f->ascent;
    const char *end = text + nbytes;
    while (text < end && *text) {
        uint32_t cp = utf8_decode(&text);
        if (cp == '\t') {
            const fnt_glyph_t *sp = f->ascii[' '];
            if (sp) x64 += sp->adv * 4;
            continue;
        }
        const fnt_glyph_t *g = font_glyph(f, cp);
        if (!g) continue;
        if (g->w) draw_glyph(s, f, g, (x64 + 32) >> 6, ybase, color);
        x64 += g->adv;
    }
    return (x64 + 32) >> 6;
}

int font_draw(surface_t *s, const font_t *f, int x, int y, const char *text, uint32_t color) {
    return font_draw_n(s, f, x, y, text, (int)strlen(text), color);
}

int font_draw_cp(surface_t *s, const font_t *f, int x, int y, uint32_t cp, uint32_t color) {
    const fnt_glyph_t *g = font_glyph(f, cp);
    if (!g) return x;
    if (g->w) draw_glyph(s, f, g, x, y + f->ascent, color);
    return x + ((g->adv + 32) >> 6);
}

void font_draw_fit(surface_t *s, const font_t *f, int x, int y, const char *text, int max_w, uint32_t color) {
    int len = (int)strlen(text);
    if (font_text_width_n(f, text, len) <= max_w) { font_draw_n(s, f, x, y, text, len, color); return; }
    const char *ell = "\xE2\x80\xA6";   /* … */
    int ew = font_text_width(f, ell);
    int n = len;
    while (n > 0 && font_text_width_n(f, text, n) + ew > max_w) n = utf8_prev(text, n);
    int ex = font_draw_n(s, f, x, y, text, n, color);
    font_draw(s, f, ex, y, ell, color);
}

int font_index_at(const font_t *f, const char *s, int px) {
    int x64 = 0;
    const char *p = s;
    while (*p) {
        const char *prev = p;
        uint32_t cp = utf8_decode(&p);
        const fnt_glyph_t *g = font_glyph(f, cp);
        int adv = g ? g->adv : 0;
        if (((x64 + adv / 2) >> 6) >= px) return (int)(prev - s);
        x64 += adv;
    }
    return (int)(p - s);
}
