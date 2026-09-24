#pragma once
/* Software 2D graphics shared by the kernel window server and libgui.
 * Pixels are 32-bit 0xAARRGGBB. Integer arithmetic only (the kernel has no FPU). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { int x, y, w, h; } rect_t;

typedef struct surface {
    uint32_t *px;
    int w, h;
    int stride;          /* in pixels */
    rect_t clip;
} surface_t;

#define RGB(r, g, b)      (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGBA(r, g, b, a)  (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define COL_A(c) (((c) >> 24) & 0xFF)
#define COL_R(c) (((c) >> 16) & 0xFF)
#define COL_G(c) (((c) >> 8) & 0xFF)
#define COL_B(c) ((c) & 0xFF)
#define WITH_ALPHA(c, a) (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

static inline rect_t mkrect(int x, int y, int w, int h) { rect_t r = { x, y, w, h }; return r; }
bool rect_intersect(rect_t a, rect_t b, rect_t *out);
rect_t rect_union(rect_t a, rect_t b);
static inline bool rect_contains(rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static inline bool rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }

void gfx_init(surface_t *s, uint32_t *px, int w, int h, int stride);
void gfx_set_clip(surface_t *s, rect_t r);      /* intersected with surface bounds */
void gfx_reset_clip(surface_t *s);

static inline uint32_t gfx_blend(uint32_t dst, uint32_t src, uint32_t a) {
    /* a: 0..255 coverage of src over dst, result opaque */
    uint32_t rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * (255 - a) + 0x800080);
    uint32_t g = ((src & 0x00FF00) * a + (dst & 0x00FF00) * (255 - a) + 0x008000);
    rb = ((rb + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
    g = ((g + ((g >> 8) & 0x00FF00)) >> 8) & 0x00FF00;
    return 0xFF000000u | rb | g;
}
uint32_t gfx_mix(uint32_t a, uint32_t b, int t);   /* t 0..255: 0 -> a, 255 -> b */
uint32_t gfx_lighten(uint32_t c, int amount);      /* amount 0..255 towards white */
uint32_t gfx_darken(uint32_t c, int amount);       /* towards black */
int gfx_luma(uint32_t c);                          /* 0..255 */

void gfx_pixel(surface_t *s, int x, int y, uint32_t c);
void gfx_fill(surface_t *s, int x, int y, int w, int h, uint32_t c);   /* honours alpha */
void gfx_clear(surface_t *s, uint32_t c);
void gfx_rect(surface_t *s, int x, int y, int w, int h, uint32_t c);   /* 1px outline */
void gfx_hline(surface_t *s, int x, int y, int w, uint32_t c);
void gfx_vline(surface_t *s, int x, int y, int h, uint32_t c);
void gfx_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c);
void gfx_line_aa(surface_t *s, int x0, int y0, int x1, int y1, uint32_t c);   /* Wu-style */
void gfx_thick_line(surface_t *s, int x0, int y0, int x1, int y1, int thick, uint32_t c);
void gfx_fill_rounded(surface_t *s, int x, int y, int w, int h, int r, uint32_t c);
void gfx_rounded_rect(surface_t *s, int x, int y, int w, int h, int r, uint32_t c);
void gfx_fill_circle(surface_t *s, int cx, int cy, int r, uint32_t c);
void gfx_circle(surface_t *s, int cx, int cy, int r, uint32_t c);
void gfx_fill_ellipse(surface_t *s, int x, int y, int w, int h, uint32_t c);
void gfx_ellipse(surface_t *s, int x, int y, int w, int h, uint32_t c);
void gfx_gradient_v(surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_gradient_h(surface_t *s, int x, int y, int w, int h, uint32_t left, uint32_t right);
void gfx_fill_triangle(surface_t *s, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c);

/* copy / composite */
void gfx_blit(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h);
void gfx_blit_alpha(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy, int w, int h);
void gfx_blit_alpha_opacity(surface_t *dst, int dx, int dy, const surface_t *src, int sx, int sy,
                            int w, int h, int opacity);
void gfx_blit_scaled(surface_t *dst, rect_t d, const surface_t *src, rect_t s, bool smooth, bool alpha);
/* draw a raw ARGB image (w*h pixels, row-major) with alpha */
void gfx_draw_image(surface_t *dst, int x, int y, const uint32_t *img, int w, int h);
void gfx_draw_image_tinted(surface_t *dst, int x, int y, const uint32_t *img, int w, int h, uint32_t tint);

/* soft drop shadow around rect r (drawn outside it) */
void gfx_shadow(surface_t *s, rect_t r, int radius, int size, int strength, int offset_y);
