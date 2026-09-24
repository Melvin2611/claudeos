/* procedural wallpapers (integer math only) and image wallpapers */
#include "wm.h"
#include <vfs.h>
#include <claudeos/image.h>

static uint32_t *wp_buf;
static surface_t wp;
static int16_t sin_tab[1024];   /* sin(i * 2pi / 1024) * 4096 */

static void build_sin(void) {
    /* Bhaskara I approximation, good to ~0.2% */
    for (int i = 0; i < 1024; i++) {
        int deg10 = i * 3600 / 1024;          /* tenths of a degree */
        int sign = 1;
        if (deg10 >= 1800) { deg10 -= 1800; sign = -1; }
        int64_t x = deg10;                    /* 0..1800 */
        int64_t num = 4 * x * (1800 - x);
        int64_t den = 4050000 - x * (1800 - x);
        sin_tab[i] = (int16_t)(sign * (num * 4096 / den));
    }
}

static inline int isin(int a) { return sin_tab[a & 1023]; }

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352D; x ^= x >> 15; x *= 0x846CA68B; x ^= x >> 16;
    return x;
}

/* smooth 1D value noise, result 0..4096 */
static int noise1(int x, int scale, uint32_t seed) {
    int i = x / scale, f = (x % scale) * 4096 / scale;
    int a = hash32(i * 374761393u + seed) & 4095;
    int b = hash32((i + 1) * 374761393u + seed) & 4095;
    int t = (f * f / 4096) * (3 * 4096 - 2 * f) / 4096;   /* smoothstep */
    return a + (b - a) * t / 4096;
}

static void render_gradient(uint32_t c1, uint32_t c2) {
    int w = wp.w, h = wp.h;
    int maxd = w + h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            wp.px[y * w + x] = gfx_mix(c1, c2, (x + y) * 255 / maxd);
}

static void render_waves(uint32_t c1, uint32_t c2) {
    int w = wp.w, h = wp.h;
    uint32_t top = gfx_darken(c1, 90), bottom = c1;
    for (int y = 0; y < h; y++) {
        uint32_t c = gfx_mix(top, bottom, y * 255 / h);
        for (int x = 0; x < w; x++) wp.px[y * w + x] = c;
    }
    const int layers = 6;
    for (int l = 0; l < layers; l++) {
        int base = h * (38 + l * 11) / 100;
        int amp = h * (7 - l / 2) / 100;
        int freq = 1 + (l % 3);
        int phase = l * 173;
        uint32_t col = gfx_mix(c1, c2, 70 + l * 185 / layers);
        if (l % 2) col = gfx_mix(col, 0xFFFFFFFF, 30);
        int alpha = 120 + l * 18;
        for (int x = 0; x < w; x++) {
            int a1 = x * 1024 * freq / w + phase;
            int yb = base + (amp * isin(a1) + amp / 2 * isin(a1 * 2 + 300 + l * 57)) / 4096;
            if (yb < 0) yb = 0;
            for (int y = yb; y < h; y++) {
                int a = alpha;
                int d = y - yb;
                if (d < 2) a = alpha * (d + 1) / 3;            /* soft edge */
                uint32_t cc = d < 40 ? gfx_mix(gfx_lighten(col, 40), col, d * 255 / 40) : col;
                wp.px[y * w + x] = gfx_blend(wp.px[y * w + x], cc, a);
            }
        }
    }
}

static void render_aurora(uint32_t c1, uint32_t c2) {
    int w = wp.w, h = wp.h;
    uint32_t top = 0xFF03060F, mid = 0xFF0A1A2C;
    for (int y = 0; y < h; y++) {
        uint32_t c = gfx_mix(top, mid, y * 255 / h);
        for (int x = 0; x < w; x++) wp.px[y * w + x] = c;
    }
    /* stars */
    for (int i = 0; i < w * h / 900; i++) {
        uint32_t r = hash32(i * 2654435761u);
        int x = r % w, y = (r >> 12) % (h * 3 / 4);
        int b = 120 + (r >> 24) % 135;
        wp.px[y * w + x] = gfx_blend(wp.px[y * w + x], 0xFFFFFFFF, b);
    }
    uint32_t green = 0xFF38F2A6;
    uint32_t col2 = c2 ? c2 : 0xFF9B5CF6;
    for (int band = 0; band < 3; band++) {
        int cy_base = h * (30 + band * 12) / 100;
        for (int x = 0; x < w; x++) {
            int a = x * 1024 / w;
            int cy = cy_base + (h / 12) * isin(a * (band + 1) + band * 200) / 4096 +
                     (h / 30) * isin(a * 5 + band * 90) / 4096;
            int intensity = 2048 + isin(a * 3 + band * 311) / 2;   /* 0..4096 */
            uint32_t col = gfx_mix(green, col2, (x * 255 / w + band * 60) % 256);
            int up = h / 4, down = h / 20;
            for (int y = cy - up; y < cy + down; y++) {
                if (y < 0 || y >= h) continue;
                int d = y < cy ? (cy - y) * 255 / up : (y - cy) * 255 / down;
                int t = 255 - d;
                int alpha = t * t / 255 * intensity / 4096 * 150 / 255;
                if (alpha > 0) wp.px[y * w + x] = gfx_blend(wp.px[y * w + x], col, alpha);
            }
        }
    }
    /* hills silhouette */
    for (int x = 0; x < w; x++) {
        int hy = h - h / 8 - (noise1(x, w / 6, 7) * (h / 10) / 4096) - (noise1(x, w / 40, 9) * (h / 60) / 4096);
        for (int y = hy; y < h; y++) wp.px[y * w + x] = 0xFF02040A;
    }
    UNUSED(c1);
}

static void render_mountains(uint32_t c1, uint32_t c2) {
    int w = wp.w, h = wp.h;
    uint32_t sky_top = c1, sky_bottom = gfx_lighten(c2, 60);
    for (int y = 0; y < h; y++) {
        uint32_t c = gfx_mix(sky_top, sky_bottom, MIN(255, y * 255 / (h * 7 / 10)));
        for (int x = 0; x < w; x++) wp.px[y * w + x] = c;
    }
    /* sun glow */
    int sx = w * 68 / 100, sy = h * 42 / 100, sr = h / 12;
    for (int y = MAX(0, sy - sr * 5); y < MIN(h, sy + sr * 5); y++) {
        for (int x = MAX(0, sx - sr * 5); x < MIN(w, sx + sr * 5); x++) {
            int dx = x - sx, dy = y - sy;
            int d2 = dx * dx + dy * dy;
            int a;
            if (d2 < sr * sr) a = 235;
            else {
                int r5 = sr * 5;
                a = d2 < r5 * r5 ? 120 - (int)((int64_t)d2 * 120 / ((int64_t)r5 * r5)) : 0;
            }
            if (a > 0) wp.px[y * w + x] = gfx_blend(wp.px[y * w + x], 0xFFFFE8C8, a);
        }
    }
    const int layers = 5;
    for (int l = 0; l < layers; l++) {
        int base = h * (48 + l * 9) / 100;
        int amp = h * (22 - l * 3) / 100;
        uint32_t col = gfx_mix(gfx_mix(sky_bottom, c1, 90), gfx_darken(c1, 150), l * 255 / (layers - 1));
        for (int x = 0; x < w; x++) {
            int n = noise1(x, w / (3 + l), 17 + l * 31) * 6 + noise1(x, w / (12 + l * 4), 101 + l) * 3 +
                    noise1(x, w / 60 + 1, 555 + l) * 1;
            int yb = base - amp * n / (4096 * 10) + amp / 3;
            for (int y = MAX(0, yb); y < h; y++) wp.px[y * w + x] = col;
        }
    }
}

static void render_bubbles(uint32_t c1, uint32_t c2) {
    render_gradient(c1, gfx_darken(c2, 60));
    int w = wp.w, h = wp.h;
    for (int i = 0; i < 38; i++) {
        uint32_t r = hash32(i * 911 + 3);
        int cx = r % w, cy = (r >> 11) % h;
        int rad = h / 40 + (int)((r >> 20) % (h / 7));
        uint32_t col = gfx_mix(c2, 0xFFFFFFFF, (r >> 3) % 160);
        int alpha = 25 + (r >> 26) % 45;
        for (int y = MAX(0, cy - rad); y < MIN(h, cy + rad); y++) {
            for (int x = MAX(0, cx - rad); x < MIN(w, cx + rad); x++) {
                int dx = x - cx, dy = y - cy;
                int d2 = dx * dx + dy * dy;
                if (d2 >= rad * rad) continue;
                int edge = rad * rad - d2;
                int a = edge < rad * 3 ? alpha * edge / (rad * 3) : alpha;
                wp.px[y * w + x] = gfx_blend(wp.px[y * w + x], col, a);
            }
        }
    }
}

static bool render_image(const char *path, int mode, uint32_t bg) {
    size_t size;
    void *data = vfs_read_all(path, &size);
    if (!data) return false;
    int iw, ih;
    if (!bmp_info(data, size, &iw, &ih)) { kfree(data); return false; }
    uint32_t *px = vmalloc((size_t)iw * ih * 4);
    bmp_decode(data, size, px);
    kfree(data);
    surface_t img;
    gfx_init(&img, px, iw, ih, iw);
    gfx_fill(&wp, 0, 0, wp.w, wp.h, bg);
    rect_t src = mkrect(0, 0, iw, ih), dst = mkrect(0, 0, wp.w, wp.h);
    switch (mode) {
    case WPM_FILL: {
        /* scale to cover, crop the overflow */
        if ((int64_t)iw * wp.h > (int64_t)ih * wp.w) {
            int cw = (int)((int64_t)ih * wp.w / wp.h);
            src = mkrect((iw - cw) / 2, 0, cw, ih);
        } else {
            int chh = (int)((int64_t)iw * wp.h / wp.w);
            src = mkrect(0, (ih - chh) / 2, iw, chh);
        }
        gfx_blit_scaled(&wp, dst, &img, src, true, false);
        break;
    }
    case WPM_FIT: {
        int dw = wp.w, dh = (int)((int64_t)ih * wp.w / iw);
        if (dh > wp.h) { dh = wp.h; dw = (int)((int64_t)iw * wp.h / ih); }
        gfx_blit_scaled(&wp, mkrect((wp.w - dw) / 2, (wp.h - dh) / 2, dw, dh), &img, src, true, false);
        break;
    }
    case WPM_STRETCH:
        gfx_blit_scaled(&wp, dst, &img, src, true, false);
        break;
    case WPM_CENTER:
        gfx_blit(&wp, (wp.w - iw) / 2, (wp.h - ih) / 2, &img, 0, 0, iw, ih);
        break;
    case WPM_TILE:
        for (int y = 0; y < wp.h; y += ih)
            for (int x = 0; x < wp.w; x += iw) gfx_blit(&wp, x, y, &img, 0, 0, iw, ih);
        break;
    }
    vfree(px);
    return true;
}

void wallpaper_render(void) {
    static bool inited;
    if (!inited) { build_sin(); inited = true; }
    if (wp_buf && (wp.w != wm.w || wp.h != wm.h)) { vfree(wp_buf); wp_buf = 0; }
    if (!wp_buf) {
        wp_buf = vmalloc((size_t)wm.w * wm.h * 4);
        gfx_init(&wp, wp_buf, wm.w, wm.h, wm.w);
    }
    uint32_t c1 = wm.cfg.wp_color1 | 0xFF000000u, c2 = wm.cfg.wp_color2 | 0xFF000000u;
    uint64_t t0 = uptime_ms();
    switch (wm.cfg.wallpaper) {
    case WP_SOLID: gfx_fill(&wp, 0, 0, wp.w, wp.h, c1); break;
    case WP_GRADIENT: render_gradient(c1, c2); break;
    case WP_AURORA: render_aurora(c1, c2); break;
    case WP_MOUNTAINS: render_mountains(c1, c2); break;
    case WP_BUBBLES: render_bubbles(c1, c2); break;
    case WP_IMAGE:
        if (render_image(wm.cfg.wp_image, wm.cfg.wp_mode, c1)) break;
        render_waves(c1, c2);
        break;
    default: render_waves(c1, c2); break;
    }
    klog("[wm] wallpaper %u rendered in %lu ms\n", wm.cfg.wallpaper, uptime_ms() - t0);
}

void wallpaper_draw(surface_t *s, rect_t r) {
    if (!wp_buf) { gfx_fill(s, r.x, r.y, r.w, r.h, wm.theme.desktop_bg); return; }
    gfx_blit(s, r.x, r.y, &wp, r.x, r.y, r.w, r.h);
}
