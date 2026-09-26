/* Decompression of btrfs extents: zlib (RFC 1950/1951 inflate), LZO1X in btrfs' segment
 * framing, and zstd (see zstd_decompress.c). */
#include <kernel.h>
#include <mm.h>
#include "btrfs.h"

/* ------------------------------------------------------------------ inflate */

typedef struct {
    const uint8_t *in, *in_end;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t out_len, out_pos;
    bool err;
} inf_t;

typedef struct { uint16_t count[16], symbol[288]; } huff_t;

static int getbit(inf_t *s) {
    if (!s->bitcnt) {
        if (s->in >= s->in_end) { s->err = true; return 0; }
        s->bitbuf = *s->in++;
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static uint32_t getbits(inf_t *s, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint32_t)getbit(s) << i;
    return v;
}

static int build(huff_t *h, const uint8_t *lens, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
    return 0;
}

static int decode(inf_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbit(s);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->err) return -1;
    }
    s->err = true;
    return -1;
}

static void put(inf_t *s, uint8_t b) {
    if (s->out_pos < s->out_len) s->out[s->out_pos] = b;
    s->out_pos++;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                       67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                        11, 11, 12, 12, 13, 13 };

static int codes(inf_t *s, const huff_t *lit, const huff_t *dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) { put(s, (uint8_t)sym); continue; }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return -1;
        int len = len_base[sym] + (int)getbits(s, len_extra[sym]);
        int ds = decode(s, dist);
        if (ds < 0 || ds >= 30) return -1;
        size_t d = dist_base[ds] + getbits(s, dist_extra[ds]);
        if (d > s->out_pos) return -1;
        while (len--) {
            size_t from = s->out_pos - d;
            put(s, from < s->out_len ? s->out[from] : 0);
        }
        if (s->err) return -1;
    }
}

static int inflate_raw(inf_t *s) {
    int last;
    do {
        last = getbit(s);
        int type = (int)getbits(s, 2);
        if (s->err) return -1;
        if (type == 0) {
            s->bitcnt = 0;
            if (s->in + 4 > s->in_end) return -1;
            uint16_t len = s->in[0] | (s->in[1] << 8);
            s->in += 4;
            if (s->in + len > s->in_end) return -1;
            while (len--) put(s, *s->in++);
        } else if (type == 1 || type == 2) {
            huff_t *lit = kmalloc(sizeof(huff_t)), *dist = kmalloc(sizeof(huff_t));
            uint8_t lens[320];
            int r;
            if (type == 1) {
                int i = 0;
                for (; i < 144; i++) lens[i] = 8;
                for (; i < 256; i++) lens[i] = 9;
                for (; i < 280; i++) lens[i] = 7;
                for (; i < 288; i++) lens[i] = 8;
                build(lit, lens, 288);
                for (i = 0; i < 30; i++) lens[i] = 5;
                build(dist, lens, 30);
            } else {
                int nlen = (int)getbits(s, 5) + 257, ndist = (int)getbits(s, 5) + 1, ncode = (int)getbits(s, 4) + 4;
                static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                uint8_t cl[19] = { 0 };
                for (int i = 0; i < ncode; i++) cl[order[i]] = (uint8_t)getbits(s, 3);
                huff_t *clh = kmalloc(sizeof(huff_t));
                build(clh, cl, 19);
                int i = 0;
                while (i < nlen + ndist && !s->err) {
                    int sym = decode(s, clh);
                    if (sym < 0) break;
                    if (sym < 16) { lens[i++] = (uint8_t)sym; continue; }
                    int rep = 0;
                    uint8_t v = 0;
                    if (sym == 16) { if (!i) { s->err = true; break; } v = lens[i - 1]; rep = 3 + (int)getbits(s, 2); }
                    else if (sym == 17) rep = 3 + (int)getbits(s, 3);
                    else rep = 11 + (int)getbits(s, 7);
                    while (rep-- && i < nlen + ndist) lens[i++] = v;
                }
                kfree(clh);
                build(lit, lens, nlen);
                build(dist, lens + nlen, ndist);
            }
            r = s->err ? -1 : codes(s, lit, dist);
            kfree(lit);
            kfree(dist);
            if (r) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return 0;
}

static int zlib_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    if (in_len < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31) return -EIO;
    inf_t s = { in + 2, in + in_len, 0, 0, out, out_len, 0, false };
    if (inflate_raw(&s) < 0) return -EIO;
    return (int)MIN(s.out_pos, out_len);
}

/* ------------------------------------------------------------------ LZO1X */

static int lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_max, size_t *out_len) {
    const uint8_t *ip = in, *ip_end = in + in_len;
    uint8_t *op = out, *op_end = out + out_max;
    const uint8_t *m_pos;
    size_t t, next;
    int state = 0;
#define NEED_IP(n) do { if ((size_t)(ip_end - ip) < (size_t)(n)) return -1; } while (0)
#define NEED_OP(n) do { if ((size_t)(op_end - op) < (size_t)(n)) return -1; } while (0)
#define TEST_LB(p) do { if ((p) < out) return -1; } while (0)
    NEED_IP(1);
    if (*ip > 17) {
        t = *ip++ - 17;
        if (t < 4) { next = t; goto match_next; }
        goto copy_literal_run;
    }
    for (;;) {
        NEED_IP(1);
        t = *ip++;
        if (t < 16) {
            if (state == 0) {
                if (t == 0) {
                    const uint8_t *ip_last = ip;
                    while (1) { NEED_IP(1); if (*ip) break; ip++; }
                    size_t off = (size_t)(ip - ip_last);
                    off = (off << 8) - off;
                    t += off + 15 + *ip++;
                }
                t += 3;
copy_literal_run:
                NEED_IP(t + 3);
                NEED_OP(t);
                while (t--) *op++ = *ip++;
                state = 4;
                continue;
            } else if (state != 4) {
                next = t & 3;
                m_pos = op - 1 - (t >> 2);
                NEED_IP(1);
                m_pos -= *ip++ << 2;
                TEST_LB(m_pos);
                NEED_OP(2);
                op[0] = m_pos[0];
                op[1] = m_pos[1];
                op += 2;
                goto match_next;
            } else {
                next = t & 3;
                m_pos = op - (1 + 0x0800) - (t >> 2);
                NEED_IP(1);
                m_pos -= *ip++ << 2;
                t = 3;
            }
        } else if (t >= 64) {
            next = t & 3;
            m_pos = op - 1 - ((t >> 2) & 7);
            NEED_IP(1);
            m_pos -= *ip++ << 3;
            t = (t >> 5) - 1 + (3 - 1);
        } else if (t >= 32) {
            t = (t & 31) + (3 - 1);
            if (t == 2) {
                const uint8_t *ip_last = ip;
                while (1) { NEED_IP(1); if (*ip) break; ip++; }
                size_t off = (size_t)(ip - ip_last);
                off = (off << 8) - off;
                t += off + 31 + *ip++;
            }
            NEED_IP(2);
            next = ip[0] | (ip[1] << 8);
            ip += 2;
            m_pos = op - 1 - (next >> 2);
            next &= 3;
        } else {
            m_pos = op - ((t & 8) << 11);
            t = (t & 7) + (3 - 1);
            if (t == 2) {
                const uint8_t *ip_last = ip;
                while (1) { NEED_IP(1); if (*ip) break; ip++; }
                size_t off = (size_t)(ip - ip_last);
                off = (off << 8) - off;
                t += off + 7 + *ip++;
            }
            NEED_IP(2);
            next = ip[0] | (ip[1] << 8);
            ip += 2;
            m_pos -= next >> 2;
            next &= 3;
            if (m_pos == op) goto eof_found;
            m_pos -= 0x4000;
        }
        TEST_LB(m_pos);
        {
            uint8_t *oe = op + t;
            NEED_OP(t);
            op[0] = m_pos[0];
            op[1] = m_pos[1];
            op += 2;
            m_pos += 2;
            while (op < oe) *op++ = *m_pos++;
        }
match_next:
        state = (int)next;
        t = next;
        NEED_IP(t + 3);
        NEED_OP(t);
        while (t--) *op++ = *ip++;
    }
eof_found:
    *out_len = (size_t)(op - out);
    return t == 3 ? 0 : -1;
#undef NEED_IP
#undef NEED_OP
#undef TEST_LB
}

/* btrfs LZO framing: total length, then (len, data) segments that never straddle a sector */
static int lzo_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, uint32_t ss) {
    if (in_len < 4) return -EIO;
    uint32_t total = rd32le(in);
    if (total > in_len) return -EIO;
    size_t ipos = 4, opos = 0;
    while (ipos + 4 <= total && opos < out_len) {
        if (ss - (ipos % ss) < 4) ipos = ALIGN_UP(ipos, ss);   /* header would cross a sector */
        if (ipos + 4 > total) break;
        uint32_t seg = rd32le(in + ipos);
        ipos += 4;
        if (ipos + seg > total) return -EIO;
        size_t got = 0;
        if (lzo1x_decompress(in + ipos, seg, out + opos, out_len - opos, &got) < 0) return -EIO;
        opos += got;
        ipos += seg;
    }
    return (int)opos;
}

/* ------------------------------------------------------------------ dispatcher */

int zstd_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

int btrfs_decompress(int type, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, uint32_t sectorsize) {
    switch (type) {
    case 1: return zlib_decompress(in, in_len, out, out_len);
    case 2: return lzo_decompress(in, in_len, out, out_len, sectorsize);
    case 3: return zstd_decompress(in, in_len, out, out_len);
    default: return -EIO;
    }
}
