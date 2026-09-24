/* Image codecs: ClaudeOS .icn and Windows BMP */
#include <claudeos/image.h>

void *memset(void *d, int c, size_t n);

bool icn_parse(const void *data, size_t size, int *w, int *h, const uint32_t **pixels) {
    const icn_header_t *hd = data;
    if (size < sizeof(icn_header_t)) return false;
    if (hd->magic[0] != 'C' || hd->magic[1] != 'I' || hd->magic[2] != 'C' || hd->magic[3] != 'N') return false;
    if ((size_t)hd->w * hd->h * 4 + sizeof(icn_header_t) > size) return false;
    *w = hd->w;
    *h = hd->h;
    *pixels = (const uint32_t *)((const uint8_t *)data + sizeof(icn_header_t));
    return true;
}

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

typedef struct {
    int w, h, bpp;
    bool bottom_up;
    uint32_t data_off, compression;
    uint32_t masks[4];
    const uint8_t *palette;
    int palette_entries;
} bmp_hdr_t;

static bool bmp_parse(const void *data, size_t size, bmp_hdr_t *b) {
    const uint8_t *p = data;
    if (size < 54 || p[0] != 'B' || p[1] != 'M') return false;
    b->data_off = rd32(p + 10);
    uint32_t hsize = rd32(p + 14);
    if (hsize < 40) return false;
    b->w = (int32_t)rd32(p + 18);
    int32_t h = (int32_t)rd32(p + 22);
    b->bottom_up = h > 0;
    b->h = h < 0 ? -h : h;
    b->bpp = rd16(p + 28);
    b->compression = rd32(p + 30);
    uint32_t colors = rd32(p + 46);
    if (b->w <= 0 || b->h <= 0 || b->w > 16384 || b->h > 16384) return false;
    if (b->bpp != 24 && b->bpp != 32 && b->bpp != 8) return false;
    if (b->compression != 0 && b->compression != 3) return false;
    b->masks[0] = 0x00FF0000; b->masks[1] = 0x0000FF00; b->masks[2] = 0x000000FF; b->masks[3] = 0xFF000000;
    if (b->compression == 3) {
        const uint8_t *m = p + 14 + 40;
        if (hsize >= 52) m = p + 54;   /* V4/V5 headers carry masks inside */
        if (14 + hsize + 12 <= size || hsize >= 52) {
            b->masks[0] = rd32(m); b->masks[1] = rd32(m + 4); b->masks[2] = rd32(m + 8);
            b->masks[3] = hsize >= 56 ? rd32(m + 12) : 0;
        }
    } else if (b->bpp == 32) {
        b->masks[3] = 0;   /* BI_RGB 32 bpp: alpha byte usually unused */
    }
    if (b->bpp == 8) {
        b->palette = p + 14 + hsize;
        b->palette_entries = colors ? (int)colors : 256;
    }
    size_t stride = ((size_t)b->w * b->bpp / 8 + 3) & ~3u;
    if (b->data_off + stride * b->h > size) return false;
    return true;
}

bool bmp_info(const void *data, size_t size, int *w, int *h) {
    bmp_hdr_t b;
    if (!bmp_parse(data, size, &b)) return false;
    *w = b.w;
    *h = b.h;
    return true;
}

static uint32_t extract(uint32_t v, uint32_t mask) {
    if (!mask) return 255;
    int shift = __builtin_ctz(mask);
    uint32_t bits = mask >> shift;
    uint32_t x = (v & mask) >> shift;
    if (bits == 255) return x;
    return bits ? x * 255 / bits : 0;
}

bool bmp_decode(const void *data, size_t size, uint32_t *out) {
    bmp_hdr_t b;
    if (!bmp_parse(data, size, &b)) return false;
    const uint8_t *p = (const uint8_t *)data + b.data_off;
    size_t stride = ((size_t)b.w * b.bpp / 8 + 3) & ~3u;
    bool has_alpha = false;
    for (int y = 0; y < b.h; y++) {
        const uint8_t *row = p + stride * (b.bottom_up ? b.h - 1 - y : y);
        uint32_t *o = out + (size_t)y * b.w;
        for (int x = 0; x < b.w; x++) {
            if (b.bpp == 24) {
                o[x] = 0xFF000000u | (row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
            } else if (b.bpp == 8) {
                int idx = row[x];
                const uint8_t *c = b.palette + (idx < b.palette_entries ? idx : 0) * 4;
                o[x] = 0xFF000000u | (c[2] << 16) | (c[1] << 8) | c[0];
            } else {
                uint32_t v = rd32(row + x * 4);
                uint32_t a = b.masks[3] ? extract(v, b.masks[3]) : 255;
                if (b.masks[3] && a) has_alpha = true;
                o[x] = (a << 24) | (extract(v, b.masks[0]) << 16) | (extract(v, b.masks[1]) << 8) |
                       extract(v, b.masks[2]);
            }
        }
    }
    if (b.bpp == 32 && b.masks[3] && !has_alpha) {
        /* alpha channel present but all zero: treat as opaque */
        for (size_t i = 0; i < (size_t)b.w * b.h; i++) out[i] |= 0xFF000000u;
    }
    return true;
}

size_t bmp_encoded_size(int w, int h) {
    size_t stride = ((size_t)w * 3 + 3) & ~3u;
    return 54 + stride * h;
}

void bmp_encode(const uint32_t *px, int w, int h, void *outv) {
    uint8_t *out = outv;
    size_t stride = ((size_t)w * 3 + 3) & ~3u;
    size_t total = 54 + stride * h;
    memset(out, 0, 54);
    out[0] = 'B'; out[1] = 'M';
    wr32(out + 2, (uint32_t)total);
    wr32(out + 10, 54);
    wr32(out + 14, 40);
    wr32(out + 18, w);
    wr32(out + 22, h);
    wr16(out + 26, 1);
    wr16(out + 28, 24);
    wr32(out + 34, (uint32_t)(stride * h));
    wr32(out + 38, 2835);
    wr32(out + 42, 2835);
    for (int y = 0; y < h; y++) {
        uint8_t *row = out + 54 + stride * (h - 1 - y);
        const uint32_t *src = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            row[x * 3] = src[x] & 0xFF;
            row[x * 3 + 1] = (src[x] >> 8) & 0xFF;
            row[x * 3 + 2] = (src[x] >> 16) & 0xFF;
        }
        for (size_t i = (size_t)w * 3; i < stride; i++) row[i] = 0;
    }
}
