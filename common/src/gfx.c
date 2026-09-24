/* Software rasterizer shared by kernel and userland (integer only). */
#include <claudeos/gfx.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }

static uint64_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

bool rect_intersect(rect_t a, rect_t b, rect_t *out) {
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int x1 = imin(a.x + a.w, b.x + b.w), y1 = imin(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) { if (out) *out = mkrect(0, 0, 0, 0); return false; }
    if (out) *out = mkrect(x0, y0, x1 - x0, y1 - y0);
    return true;
}

rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = imin(a.x, b.x), y0 = imin(a.y, b.y);
    int x1 = imax(a.x + a.w, b.x + b.w), y1 = imax(a.y + a.h, b.y + b.h);
    return mkrect(x0, y0, x1 - x0, y1 - y0);
}

void gfx_init(surface_t *s, uint32_t *px, int w, int h, int stride) {
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride;
    s->clip = mkrect(0, 0, w, h);
}

void gfx_set_clip(surface_t *s, rect_t r) {
    if (!rect_intersect(r, mkrect(0, 0, s->w, s->h), &s->clip)) s->clip = mkrect(0, 0, 0, 0);
}

void gfx_reset_clip(surface_t *s) { s->clip = mkrect(0, 0, s->w, s->h); }

uint32_t gfx_mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 255) return b;
    uint32_t ra = COL_R(a), ga = COL_G(a), ba = COL_B(a), aa = COL_A(a);
    uint32_t r = ra + ((int)(COL_R(b) - ra) * t) / 255;
    uint32_t g = ga + ((int)(COL_G(b) - ga) * t) / 255;
    uint32_t bl = ba + ((int)(COL_B(b) - ba) * t) / 255;
    uint32_t al = aa + ((int)(COL_A(b) - aa) * t) / 255;
    return RGBA(r, g, bl, al);
}

uint32_t gfx_lighten(uint32_t c, int amount) { return gfx_mix(c, WITH_ALPHA(0xFFFFFF, COL_A(c)), amount); }
uint32_t gfx_darken(uint32_t c, int amount) { return gfx_mix(c, WITH_ALPHA(0x000000, COL_A(c)), amount); }
int gfx_luma(uint32_t c) { return (COL_R(c) * 77 + COL_G(c) * 150 + COL_B(c) * 29) >> 8; }

static inline void put(surface_t *s, int x, int y, uint32_t c, int cov) {
    if (x < s->clip.x || y < s->clip.y || x >= s->clip.x + s->clip.w || y >= s->clip.y + s->clip.h) return;
    uint32_t a = (COL_A(c) * (uint32_t)cov + 127) / 255;
    uint32_t *p = &s->px[y * s->stride + x];
    if (a >= 255) *p = c | 0xFF000000u;
    else if (a > 0) *p = gfx_blend(*p, c, a);
}

void gfx_pixel(surface_t *s, int x, int y, uint32_t c) { put(s, x, y, c, 255); }

void gfx_fill(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    rect_t r;
    if (!rect_intersect(mkrect(x, y, w, h), s->clip, &r)) return;
    uint32_t a = COL_A(c);
    if (a == 0) return;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        uint32_t *p = &s->px[yy * s->stride + r.x];
        if (a == 255) {
            int n = r.w;
            uint32_t v = c;
            while (n >= 4) { p[0] = v; p[1] = v; p[2] = v; p[3] = v; p += 4; n -= 4; }
            while (n--) *p++ = v;
        } else {
            for (int i = 0; i < r.w; i++) p[i] = gfx_blend(p[i], c, a);
        }
    }
}

void gfx_clear(surface_t *s, uint32_t c) {
    rect_t old = s->clip;
    gfx_reset_clip(s);
    gfx_fill(s, 0, 0, s->w, s->h, c | 0xFF000000u);
    s->clip = old;
}

void gfx_hline(surface_t *s, int x, int y, int w, uint32_t c) { gfx_fill(s, x, y, w, 1, c); }
void gfx_vline(surface_t *s, int x, int y, int h, uint32_t c) { gfx_fill(s, x, y, 1, h, c); }

void gfx_rect(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    gfx_hline(s, x, y, w, c);
    if (h > 1) gfx_hline(s, x, y + h - 1, w, c);
    if (h > 2) {
        gfx_vline(s, x, y + 1, h - 2, c);
        if (w > 1) gfx_vline(s, x + w - 1, y + 1, h - 2, c);
    }
}

void gfx_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put(s, x0, y0, c, 255);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Xiaolin Wu anti-aliased line, 8-bit fixed point */
void gfx_line_aa(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c) {
    bool steep = iabs(y1 - y0) > iabs(x1 - x0);
    if (steep) { int t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    int dx = x1 - x0, dy = y1 - y0;
    int grad = dx == 0 ? 256 : (dy * 256) / dx;   /* 8.8 */
    int y = y0 * 256;
    for (int x = x0; x <= x1; x++) {
        int yi = y >> 8, frac = y & 255;
        if (steep) {
            put(s, yi, x, c, 255 - frac);
            put(s, yi + 1, x, c, frac);
        } else {
            put(s, x, yi, c, 255 - frac);
            put(s, x, yi + 1, c, frac);
        }
        y += grad;
    }
}

void gfx_thick_line(surface_t *s, int x0, int y0, int x1, int y1, int thick, uint32_t c) {
    if (thick <= 1) { gfx_line(s, x0, y0, x1, y1, c); return; }
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int r = thick / 2;
    for (;;) {
        if (thick <= 2) gfx_fill(s, x0 - r, y0 - r, thick, thick, c);
        else gfx_fill_circle(s, x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---- ellipses: coverage in 0..255 for the pixel (px,py) of ellipse box (x,y,w,h) */
static int ellipse_cov(int px, int py, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    int64_t X = 2 * px + 1 - (2 * x + w);
    int64_t Y = 2 * py + 1 - (2 * y + h);
    int64_t A = w, B = h;
    uint64_t n2 = (uint64_t)((X * X * 65536) / (A * A) + (Y * Y * 65536) / (B * B));
    int64_t dn = (int64_t)isqrt64(n2);          /* normalized distance * 256 */
    int64_t m = w < h ? w : h;
    int64_t dist = ((256 - dn) * m) / 2;         /* distance to edge in 1/256 px */
    int64_t cov = dist + 128;
    if (cov <= 0) return 0;
    if (cov >= 256) return 255;
    return (int)(cov * 255 / 256);
}

void gfx_fill_ellipse(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    rect_t r;
    if (!rect_intersect(mkrect(x, y, w, h), s->clip, &r)) return;
    for (int py = r.y; py < r.y + r.h; py++)
        for (int px = r.x; px < r.x + r.w; px++) {
            int cv = ellipse_cov(px, py, x, y, w, h);
            if (cv) put(s, px, py, c, cv);
        }
}

void gfx_ellipse(surface_t *s, int x, int y, int w, int h, uint32_t c) {
    rect_t r;
    if (!rect_intersect(mkrect(x, y, w, h), s->clip, &r)) return;
    for (int py = r.y; py < r.y + r.h; py++)
        for (int px = r.x; px < r.x + r.w; px++) {
            int cv = ellipse_cov(px, py, x, y, w, h) - ellipse_cov(px, py, x + 1, y + 1, w - 2, h - 2);
            if (cv > 0) put(s, px, py, c, cv);
        }
}

void gfx_fill_circle(surface_t *s, int cx, int cy, int r, uint32_t c) {
    gfx_fill_ellipse(s, cx - r, cy - r, 2 * r + 1, 2 * r + 1, c);
}

void gfx_circle(surface_t *s, int cx, int cy, int r, uint32_t c) {
    gfx_ellipse(s, cx - r, cy - r, 2 * r + 1, 2 * r + 1, c);
}

/* coverage of pixel (px,py) by the rounded rect (x,y,w,h,r) */
static int rrect_cov(int px, int py, int x, int y, int w, int h, int r) {
    if (px < x || py < y || px >= x + w || py >= y + h) return 0;
    if (r <= 0) return 255;
    int cx, cy;
    if (px < x + r) cx = x + r;
    else if (px >= x + w - r) cx = x + w - r;
    else return 255;
    if (py < y + r) cy = y + r;
    else if (py >= y + h - r) cy = y + h - r;
    else return 255;
    /* distance from pixel center to corner circle center (circle center lies on pixel grid corner) */
    int64_t dx = 2 * px + 1 - 2 * cx, dy = 2 * py + 1 - 2 * cy;   /* half-pixel units */
    int64_t d256 = (int64_t)isqrt64((uint64_t)(dx * dx + dy * dy) * 65536) / 2;
    int64_t cov = (int64_t)r * 256 - d256 + 128;
    if (cov <= 0) return 0;
    if (cov >= 256) return 255;
    return (int)(cov * 255 / 256);
}

void gfx_fill_rounded(surface_t *s, int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_fill(s, x, y, w, h, c); return; }
    /* middle band */
    gfx_fill(s, x, y + r, w, h - 2 * r, c);
    /* top and bottom bands with corners */
    for (int band = 0; band < 2; band++) {
        int y0 = band == 0 ? y : y + h - r;
        gfx_fill(s, x + r, y0, w - 2 * r, r, c);
        for (int py = y0; py < y0 + r; py++) {
            for (int px = x; px < x + r; px++) put(s, px, py, c, rrect_cov(px, py, x, y, w, h, r));
            for (int px = x + w - r; px < x + w; px++) put(s, px, py, c, rrect_cov(px, py, x, y, w, h, r));
        }
    }
}

void gfx_rounded_rect(surface_t *s, int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_rect(s, x, y, w, h, c); return; }
    gfx_hline(s, x + r, y, w - 2 * r, c);
    gfx_hline(s, x + r, y + h - 1, w - 2 * r, c);
    gfx_vline(s, x, y + r, h - 2 * r, c);
    gfx_vline(s, x + w - 1, y + r, h - 2 * r, c);
    int ri = r - 1;
    for (int band = 0; band < 2; band++) {
        int y0 = band == 0 ? y : y + h - r;
        for (int py = y0; py < y0 + r; py++) {
            for (int side = 0; side < 2; side++) {
                int x0 = side == 0 ? x : x + w - r;
                for (int px = x0; px < x0 + r; px++) {
                    int cv = rrect_cov(px, py, x, y, w, h, r) - rrect_cov(px, py, x + 1, y + 1, w - 2, h - 2, ri);
                    if (cv > 0) put(s, px, py, c, cv);
                }
            }
        }
    }
}

void gfx_gradient_v(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (h <= 0) return;
    for (int i = 0; i < h; i++) {
        int yy = y + i;
        if (yy < s->clip.y || yy >= s->clip.y + s->clip.h) continue;
        gfx_fill(s, x, yy, w, 1, gfx_mix(top, bottom, h > 1 ? i * 255 / (h - 1) : 0));
    }
}

void gfx_gradient_h(surface_t *s, int x, int y, int w, int h, uint32_t left, uint32_t right) {
    if (w <= 0) return;
    for (int i = 0; i < w; i++) {
        int xx = x + i;
        if (xx < s->clip.x || xx >= s->clip.x + s->clip.w) continue;
        gfx_fill(s, xx, y, 1, h, gfx_mix(left, right, w > 1 ? i * 255 / (w - 1) : 0));
    }
}

void gfx_fill_triangle(surface_t *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    int minx = imin(x0, imin(x1, x2)), maxx = imax(x0, imax(x1, x2));
    int miny = imin(y0, imin(y1, y2)), maxy = imax(y0, imax(y1, y2));
    rect_t r;
    if (!rect_intersect(mkrect(minx, miny, maxx - minx + 1, maxy - miny + 1), s->clip, &r)) return;
    int area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0) return;
    for (int py = r.y; py < r.y + r.h; py++) {
        for (int px = r.x; px < r.x + r.w; px++) {
            int w0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
            int w1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
            int w2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) || (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0))
                put(s, px, py, c, 255);
        }
    }
}

/* ------------------------------------------------------------------ blits */

static bool clip_blit(surface_t *dst, int *dx, int *dy, const surface_t *src, int *sx, int *sy, int *w, int *h) {
    if (*sx < 0) { *w += *sx; *dx -= *sx; *sx = 0; }
    if (*sy < 0) { *h += *sy; *dy -= *sy; *sy = 0; }
    if (*sx + *w > src->w) *w = src->w - *sx;
    if (*sy + *h > src->h) *h = src->h - *sy;
    rect_t c = dst->clip;
    if (*dx < c.x) { int d = c.x - *dx; *w -= d; *sx += d; *dx = c.x; }
    if (*dy < c.y) { int d = c.y - *dy; *h -= d; *sy += d; *dy = c.y; }
    if (*dx + *w > c.x + c.w) *w = c.x + c.w - *dx;
    if (*dy + *h > c.y + c.h) *h = c.y + c.h - *dy;
    return *w > 0 && *h > 0;
}

void gfx_blit(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h) {
    if (!clip_blit(dst, &dx, &dy, src, &sx, &sy, &w, &h)) return;
    for (int i = 0; i < h; i++)
        memcpy(&dst->px[(dy + i) * dst->stride + dx], &src->px[(sy + i) * src->stride + sx], w * 4);
}

void gfx_blit_alpha_opacity(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy,
                            int w, int h, int opacity) {
    if (!clip_blit(dst, &dx, &dy, src, &sx, &sy, &w, &h)) return;
    for (int i = 0; i < h; i++) {
        uint32_t *d = &dst->px[(dy + i) * dst->stride + dx];
        const uint32_t *sp = &src->px[(sy + i) * src->stride + sx];
        for (int j = 0; j < w; j++) {
            uint32_t c = sp[j];
            uint32_t a = COL_A(c);
            if (opacity < 255) a = (a * opacity + 127) / 255;
            if (a == 255) d[j] = c;
            else if (a) d[j] = gfx_blend(d[j], c, a);
        }
    }
}

void gfx_blit_alpha(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h) {
    gfx_blit_alpha_opacity(dst, dx, dy, src, sx, sy, w, h, 255);
}

void gfx_draw_image(surface_t *dst, int x, int y, const uint32_t *img, int w, int h) {
    surface_t s;
    gfx_init(&s, (uint32_t *)img, w, h, w);
    gfx_blit_alpha(dst, x, y, &s, 0, 0, w, h);
}

void gfx_draw_image_tinted(surface_t *dst, int x, int y, const uint32_t *img, int w, int h, uint32_t tint) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t a = COL_A(img[j * w + i]);
            if (a) put(dst, x + i, y + j, tint, a);
        }
}

void gfx_blit_scaled(surface_t *dst, rect_t d, const surface_t *src, rect_t s, bool smooth, bool alpha) {
    if (d.w <= 0 || d.h <= 0 || s.w <= 0 || s.h <= 0) return;
    rect_t r;
    if (!rect_intersect(d, dst->clip, &r)) return;
    uint32_t stepx = (uint32_t)(((uint64_t)s.w << 16) / d.w);
    uint32_t stepy = (uint32_t)(((uint64_t)s.h << 16) / d.h);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t fy = (uint32_t)(y - d.y) * stepy + (smooth ? (stepy >> 1) - 0x8000 : 0);
        if ((int32_t)fy < 0) fy = 0;
        int sy0 = s.y + (fy >> 16);
        int sy1 = sy0 + 1 < s.y + s.h ? sy0 + 1 : sy0;
        uint32_t wy = (fy >> 8) & 0xFF;
        uint32_t *dp = &dst->px[y * dst->stride];
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t fx = (uint32_t)(x - d.x) * stepx + (smooth ? (stepx >> 1) - 0x8000 : 0);
            if ((int32_t)fx < 0) fx = 0;
            int sx0 = s.x + (fx >> 16);
            uint32_t c;
            if (smooth) {
                int sx1 = sx0 + 1 < s.x + s.w ? sx0 + 1 : sx0;
                uint32_t wx = (fx >> 8) & 0xFF;
                uint32_t c00 = src->px[sy0 * src->stride + sx0], c01 = src->px[sy0 * src->stride + sx1];
                uint32_t c10 = src->px[sy1 * src->stride + sx0], c11 = src->px[sy1 * src->stride + sx1];
                c = gfx_mix(gfx_mix(c00, c01, wx), gfx_mix(c10, c11, wx), wy);
            } else {
                c = src->px[sy0 * src->stride + sx0];
            }
            if (alpha) {
                uint32_t a = COL_A(c);
                if (a == 255) dp[x] = c;
                else if (a) dp[x] = gfx_blend(dp[x], c, a);
            } else {
                dp[x] = c | 0xFF000000u;
            }
        }
    }
}

void gfx_shadow(surface_t *s, rect_t r, int radius, int size, int strength, int offset_y) {
    if (size <= 0 || strength <= 0) return;
    rect_t win = r;
    r.y += offset_y;
    rect_t outer = mkrect(r.x - size, r.y - size, r.w + 2 * size, r.h + 2 * size);
    rect_t cl;
    if (!rect_intersect(outer, s->clip, &cl)) return;
    uint8_t lut[128];
    int lsz = size < 127 ? size : 127;
    for (int i = 0; i <= lsz; i++) {
        int t = 255 - i * 255 / lsz;      /* 255 at edge -> 0 at size */
        lut[i] = (uint8_t)((t * t / 255) * strength / 255);
    }
    int rad = radius;
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        uint32_t *row = &s->px[y * s->stride];
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            int dx = 0, dy = 0;
            if (x < r.x + rad) dx = r.x + rad - x;
            else if (x >= r.x + r.w - rad) dx = x - (r.x + r.w - 1 - rad);
            if (y < r.y + rad) dy = r.y + rad - y;
            else if (y >= r.y + r.h - rad) dy = y - (r.y + r.h - 1 - rad);
            int d;
            if (dx == 0) d = dy - rad;
            else if (dy == 0) d = dx - rad;
            else d = (int)isqrt64((uint64_t)(dx * dx + dy * dy)) - rad;
            if (d < 0) {
                /* inside the shadow rect: pixels under the window itself will be covered anyway */
                if (y >= win.y + rad && y < win.y + win.h - rad && x >= win.x && x < win.x + win.w) {
                    x = win.x + win.w - 1;
                    continue;
                }
                d = 0;
            }
            if (d >= lsz) continue;
            uint32_t a = lut[d];
            if (a) row[x] = gfx_blend(row[x], 0xFF000000u, a);
        }
    }
}
