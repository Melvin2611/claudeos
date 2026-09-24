#pragma once
#include <stdint.h>

#define MB2_BOOTLOADER_MAGIC 0x36D76289

#define MB2_TAG_END          0
#define MB2_TAG_CMDLINE      1
#define MB2_TAG_BOOTLOADER   2
#define MB2_TAG_MODULE       3
#define MB2_TAG_BASIC_MEM    4
#define MB2_TAG_MMAP         6
#define MB2_TAG_FRAMEBUFFER  8
#define MB2_TAG_ACPI_OLD     14
#define MB2_TAG_ACPI_NEW     15

typedef struct { uint32_t total_size, reserved; } mb2_info_t;
typedef struct { uint32_t type, size; } mb2_tag_t;

typedef struct { uint32_t type, size; char string[]; } mb2_tag_string_t;
typedef struct { uint32_t type, size, mod_start, mod_end; char cmdline[]; } mb2_tag_module_t;

typedef struct { uint64_t addr, len; uint32_t type, reserved; } mb2_mmap_entry_t;
typedef struct { uint32_t type, size, entry_size, entry_version; mb2_mmap_entry_t entries[]; } mb2_tag_mmap_t;

typedef struct {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t bpp, fb_type;
    uint16_t reserved;
    /* for fb_type 1 (RGB): */
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} __attribute__((packed)) mb2_tag_fb_t;

typedef struct { uint32_t type, size; uint8_t rsdp[]; } mb2_tag_acpi_t;
