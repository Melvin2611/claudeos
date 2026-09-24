#pragma once
#include <kernel.h>

#define BOOT_MAX_MMAP 128
#define BOOT_MAX_MODULES 8

typedef struct { uint64_t addr, len; uint32_t type; } boot_mmap_t;
typedef struct { uint64_t start, end; char name[64]; } boot_module_t;

typedef struct {
    boot_mmap_t mmap[BOOT_MAX_MMAP];
    int mmap_count;
    boot_module_t modules[BOOT_MAX_MODULES];
    int module_count;
    char cmdline[256];
    char loader[64];
    uint64_t mbi_phys, mbi_size;
    /* framebuffer */
    uint64_t fb_addr;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint8_t fb_type, fb_rpos, fb_rsize, fb_gpos, fb_gsize, fb_bpos, fb_bsize;
    /* ACPI RSDP copy */
    uint8_t rsdp[64];
    bool has_rsdp;
} bootinfo_t;

extern bootinfo_t bootinfo;
bool cmdline_has(const char *opt);
const char *cmdline_get(const char *key);   /* value of key=value, static buffer */
