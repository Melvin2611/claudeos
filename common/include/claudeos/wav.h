#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint32_t rate;
    uint16_t channels;
    uint16_t bits;          /* 8 or 16 */
    const uint8_t *data;    /* PCM samples */
    uint32_t data_size;     /* bytes */
    uint32_t frames;
} wav_info_t;

bool wav_parse(const void *file, size_t size, wav_info_t *out);
/* number of 48 kHz frames the converted sound will have */
uint32_t wav_out_frames(const wav_info_t *w);
/* convert to 48 kHz interleaved stereo signed 16-bit (linear interpolation); out needs wav_out_frames()*2 samples */
void wav_convert(const wav_info_t *w, int16_t *out);
