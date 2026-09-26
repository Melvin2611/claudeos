/* PNG / JPEG / GIF / BMP / TGA decoding through the vendored stb_image (public domain) */
#include <gui.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "../stb/stb_image.h"

/* decode an image file in memory into 0xAARRGGBB pixels (malloc'd) */
uint32_t *ui_decode_image(const void *data, size_t len, int *w, int *h) {
    int n;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, w, h, &n, 4);
    if (!rgba) return 0;
    size_t count = (size_t)*w * *h;
    uint32_t *px = (uint32_t *)rgba;
    for (size_t i = 0; i < count; i++) {
        unsigned char *p = rgba + i * 4;
        px[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    return px;
}

/* area-averaging / bilinear resize into a new buffer */
uint32_t *ui_scale_image(const uint32_t *src, int sw, int sh, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return 0;
    uint32_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return 0;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned a = 0, r = 0, g = 0, b = 0, n = 0;
            int stepy = (y1 - y0 + 3) / 4, stepx = (x1 - x0 + 3) / 4;
            for (int yy = y0; yy < y1 && yy < sh; yy += stepy)
                for (int xx = x0; xx < x1 && xx < sw; xx += stepx) {
                    uint32_t c = src[(size_t)yy * sw + xx];
                    a += c >> 24; r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; n++;
                }
            if (!n) n = 1;
            dst[(size_t)y * dw + x] = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return dst;
}
