/* RIFF WAVE parsing and conversion to 48 kHz stereo s16 (integer math only) */
#include <claudeos/wav.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool wav_parse(const void *file, size_t size, wav_info_t *out) {
    const uint8_t *p = file;
    if (size < 44 || p[0] != 'R' || p[1] != 'I' || p[2] != 'F' || p[3] != 'F' || p[8] != 'W' || p[9] != 'A' ||
        p[10] != 'V' || p[11] != 'E')
        return false;
    size_t off = 12;
    bool have_fmt = false;
    while (off + 8 <= size) {
        uint32_t len = rd32(p + off + 4);
        const uint8_t *c = p + off + 8;
        if (off + 8 + len > size) len = (uint32_t)(size - off - 8);
        if (!__builtin_memcmp(p + off, "fmt ", 4) && len >= 16) {
            uint16_t fmt = rd16(c);
            if (fmt != 1 && fmt != 0xFFFE) return false;   /* PCM only */
            out->channels = rd16(c + 2);
            out->rate = rd32(c + 4);
            out->bits = rd16(c + 14);
            have_fmt = true;
        } else if (!__builtin_memcmp(p + off, "data", 4) && have_fmt) {
            if (out->channels < 1 || out->channels > 2 || (out->bits != 8 && out->bits != 16) ||
                out->rate < 4000 || out->rate > 192000)
                return false;
            out->data = c;
            out->data_size = len;
            out->frames = len / (out->channels * out->bits / 8);
            return true;
        }
        off += 8 + len + (len & 1);
    }
    return false;
}

uint32_t wav_out_frames(const wav_info_t *w) {
    return (uint32_t)((uint64_t)w->frames * 48000 / w->rate);
}

static int32_t sample(const wav_info_t *w, uint32_t frame, int ch) {
    if (frame >= w->frames) frame = w->frames ? w->frames - 1 : 0;
    int c = w->channels == 2 ? ch : 0;
    if (w->bits == 16) {
        const uint8_t *s = w->data + (frame * w->channels + c) * 2;
        return (int16_t)(s[0] | (s[1] << 8));
    }
    return ((int32_t)w->data[frame * w->channels + c] - 128) << 8;
}

void wav_convert(const wav_info_t *w, int16_t *out) {
    uint32_t n = wav_out_frames(w);
    uint64_t step = ((uint64_t)w->rate << 16) / 48000;   /* 16.16 source frames per output frame */
    uint64_t pos = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t f = (uint32_t)(pos >> 16);
        int32_t frac = (int32_t)(pos & 0xFFFF);
        for (int ch = 0; ch < 2; ch++) {
            int32_t a = sample(w, f, ch), b = sample(w, f + 1, ch);
            out[i * 2 + ch] = (int16_t)(a + (((b - a) * frac) >> 16));
        }
        pos += step;
    }
}
