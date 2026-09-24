#include <kernel.h>
#include <multiboot2.h>
#include "drivers.h"

void kmain(uint32_t magic, uint64_t mbi_phys) {
    serial_init();
    kprintf("\n[boot] ClaudeOS kernel starting (magic=%x mbi=%lx)\n", magic, mbi_phys);
    if (magic != MB2_BOOTLOADER_MAGIC) panic("not booted by a multiboot2 loader");

    mb2_info_t *info = P2V(mbi_phys);
    for (mb2_tag_t *tag = (mb2_tag_t *)(info + 1); tag->type != MB2_TAG_END;
         tag = (mb2_tag_t *)((uint8_t *)tag + ((tag->size + 7) & ~7u))) {
        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            mb2_tag_fb_t *fb = (mb2_tag_fb_t *)tag;
            kprintf("[boot] framebuffer %ux%u bpp=%u pitch=%u at %lx\n", fb->width, fb->height, fb->bpp,
                    fb->pitch, fb->addr);
            uint8_t *p = P2V(fb->addr);
            for (uint32_t y = 0; y < fb->height; y++) {
                uint32_t *row = (uint32_t *)(p + y * fb->pitch);
                for (uint32_t x = 0; x < fb->width; x++)
                    row[x] = ((x * 255 / fb->width) << 16) | ((y * 255 / fb->height) << 8) | 0x80;
            }
        }
    }
    kprintf("[boot] done\n");
    for (;;) hlt();
}

NORETURN void panic(const char *fmt, ...) {
    cli();
    va_list ap;
    va_start(ap, fmt);
    kprintf("\n*** KERNEL PANIC: ");
    kvprintf(fmt, ap);
    kprintf("\n");
    va_end(ap);
    for (;;) hlt();
}

__attribute__((weak)) void *kmalloc(size_t n) { UNUSED(n); return 0; }
