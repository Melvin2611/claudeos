#pragma once
#include <kernel.h>
#include <claudeos/gfx.h>
#include <claudeos/font.h>

typedef struct {
    uint64_t phys;
    uint8_t *virt;
    uint32_t width, height, pitch, bpp;
    uint8_t rpos, gpos, bpos;
    bool native32;         /* 32 bpp with 0x00RRGGBB layout -> plain memcpy */
    bool present;
} framebuffer_t;

extern framebuffer_t fb;

void fb_init(void);
bool fb_set_mode(uint32_t w, uint32_t h);     /* Bochs/QEMU VBE only; false if unsupported */
bool fb_can_set_mode(void);
void fb_flush(const surface_t *src, rect_t r);  /* copy rect of a screen-sized surface to the screen */

/* kernel fonts (embedded) */
extern font_t kfont_ui, kfont_ui_bold, kfont_ui_large, kfont_title, kfont_display, kfont_mono, kfont_mono_bold;
void kfonts_init(void);

/* boot splash / console */
void bootcon_init(void);
void bootcon_status(const char *msg);
void bootcon_disable(void);
