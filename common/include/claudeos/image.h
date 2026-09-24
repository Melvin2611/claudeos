#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ClaudeOS icon: "CICN", u16 width, u16 height, then width*height ARGB32 pixels */
typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t w, h;
} icn_header_t;

/* Returns true and sets w/h if data is a valid .icn; pixels points into data. */
bool icn_parse(const void *data, size_t size, int *w, int *h, const uint32_t **pixels);

/* BMP (24/32-bit uncompressed or BITFIELDS) */
bool bmp_info(const void *data, size_t size, int *w, int *h);
bool bmp_decode(const void *data, size_t size, uint32_t *out);   /* out: w*h ARGB */
size_t bmp_encoded_size(int w, int h);                            /* 24-bit BMP */
void bmp_encode(const uint32_t *px, int w, int h, void *out);
