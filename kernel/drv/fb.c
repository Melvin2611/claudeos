/* Linear framebuffer driver, Bochs VBE (DISPI) mode switching, boot splash, panic screen */
#include <kernel.h>
#include <boot.h>
#include <mm.h>
#include <fb.h>

framebuffer_t fb;
font_t kfont_ui, kfont_ui_bold, kfont_ui_large, kfont_title, kfont_display, kfont_mono, kfont_mono_bold;

#define DEF_FONT(name) extern const uint8_t name##_start[], name##_end[]
DEF_FONT(font_ui); DEF_FONT(font_ui_bold); DEF_FONT(font_ui_large); DEF_FONT(font_title);
DEF_FONT(font_display); DEF_FONT(font_mono); DEF_FONT(font_mono_bold);

void kfonts_init(void) {
#define LOAD(f, n) font_load(&f, n##_start, n##_end - n##_start)
    LOAD(kfont_ui, font_ui);
    LOAD(kfont_ui_bold, font_ui_bold);
    LOAD(kfont_ui_large, font_ui_large);
    LOAD(kfont_title, font_title);
    LOAD(kfont_display, font_display);
    LOAD(kfont_mono, font_mono);
    LOAD(kfont_mono_bold, font_mono_bold);
}

/* ---- Bochs / QEMU / VirtualBox VBE DISPI interface ---- */
#define VBE_INDEX 0x1CE
#define VBE_DATA  0x1CF
static uint16_t vbe_read(uint16_t i) { outw(VBE_INDEX, i); return inw(VBE_DATA); }
static void vbe_write(uint16_t i, uint16_t v) { outw(VBE_INDEX, i); outw(VBE_DATA, v); }
static bool has_dispi;
static size_t fb_mapped_size;

static void fb_map(void) {
    size_t need = (size_t)fb.pitch * fb.height;
    if (fb.virt && need <= fb_mapped_size) return;
    if (fb.virt) iounmap(fb.virt);
    size_t sz = has_dispi ? MAX(need, (size_t)16 << 20) : need;
    fb.virt = ioremap(fb.phys, sz, CACHE_WC);
    fb_mapped_size = sz;
}

void fb_init(void) {
    if (!bootinfo.fb_addr || bootinfo.fb_type != 1) {
        klog("[fb] no linear RGB framebuffer provided by the boot loader\n");
        return;
    }
    fb.phys = bootinfo.fb_addr;
    fb.width = bootinfo.fb_width;
    fb.height = bootinfo.fb_height;
    fb.pitch = bootinfo.fb_pitch;
    fb.bpp = bootinfo.fb_bpp;
    fb.rpos = bootinfo.fb_rpos;
    fb.gpos = bootinfo.fb_gpos;
    fb.bpos = bootinfo.fb_bpos;
    fb.native32 = fb.bpp == 32 && fb.rpos == 16 && fb.gpos == 8 && fb.bpos == 0;
    uint16_t id = vbe_read(0);
    has_dispi = id >= 0xB0C0 && id <= 0xB0C5 && !cmdline_has("nomodeset");
    /* only trust DISPI if its current mode matches the boot framebuffer */
    if (has_dispi && (vbe_read(1) != fb.width || vbe_read(2) != fb.height)) has_dispi = false;
    fb_map();
    fb.present = true;
    klog("[fb] %ux%u %ubpp pitch %u at %lx%s\n", fb.width, fb.height, fb.bpp, fb.pitch, fb.phys,
         has_dispi ? " (VBE DISPI mode switching available)" : "");
}

bool fb_can_set_mode(void) { return has_dispi; }

bool fb_set_mode(uint32_t w, uint32_t h) {
    if (!has_dispi || w < 640 || h < 480 || w > 2560 || h > 1600) return false;
    if ((uint64_t)w * h * 4 > (16u << 20)) return false;
    vbe_write(4, 0);            /* disable */
    vbe_write(1, w);
    vbe_write(2, h);
    vbe_write(3, 32);
    vbe_write(4, 0x41);         /* enable | LFB */
    if (vbe_read(1) != w || vbe_read(2) != h) return false;
    fb.width = w;
    fb.height = h;
    fb.bpp = 32;
    fb.pitch = vbe_read(6) * 4;
    if (fb.pitch < w * 4) fb.pitch = w * 4;
    fb.native32 = true;
    fb_map();
    klog("[fb] mode set to %ux%u\n", w, h);
    return true;
}

void fb_flush(const surface_t *src, rect_t r) {
    if (!fb.present) return;
    rect_t clip;
    if (!rect_intersect(r, mkrect(0, 0, MIN((int)fb.width, src->w), MIN((int)fb.height, src->h)), &clip)) return;
    for (int y = clip.y; y < clip.y + clip.h; y++) {
        const uint32_t *s = &src->px[y * src->stride + clip.x];
        uint8_t *d = fb.virt + (uint64_t)y * fb.pitch;
        if (fb.native32) {
            memcpy(d + clip.x * 4, s, clip.w * 4);
        } else if (fb.bpp == 32 || fb.bpp == 24) {
            int bytes = fb.bpp / 8;
            uint8_t *p = d + clip.x * bytes;
            for (int x = 0; x < clip.w; x++, p += bytes) {
                uint32_t c = s[x];
                uint32_t v = (COL_R(c) << fb.rpos) | (COL_G(c) << fb.gpos) | (COL_B(c) << fb.bpos);
                p[0] = v; p[1] = v >> 8; p[2] = v >> 16;
                if (bytes == 4) p[3] = 0;
            }
        } else if (fb.bpp == 16) {
            uint16_t *p = (uint16_t *)(d + clip.x * 2);
            for (int x = 0; x < clip.w; x++) {
                uint32_t c = s[x];
                p[x] = ((COL_R(c) >> 3) << 11) | ((COL_G(c) >> 2) << 5) | (COL_B(c) >> 3);
            }
        }
    }
}

/* ------------------------------------------------------------------ boot splash */

static surface_t con_surf;
static uint32_t *con_buf;
static bool con_active;
static char con_status[128];

static void splash_draw(void) {
    int w = fb.width, h = fb.height;
    surface_t *s = &con_surf;
    gfx_gradient_v(s, 0, 0, w, h, RGB(18, 20, 32), RGB(30, 34, 54));
    /* logo: rounded square with a "C" */
    int ls = 96, lx = (w - ls) / 2, ly = h / 2 - 110;
    gfx_shadow(s, mkrect(lx, ly, ls, ls), 22, 18, 120, 6);
    gfx_fill_rounded(s, lx, ly, ls, ls, 22, RGB(217, 119, 87));
    gfx_fill_circle(s, lx + ls / 2, ly + ls / 2, 30, RGB(255, 255, 255));
    gfx_fill_circle(s, lx + ls / 2, ly + ls / 2, 18, RGB(217, 119, 87));
    gfx_fill(s, lx + ls / 2 + 4, ly + ls / 2 - 9, 30, 18, RGB(217, 119, 87));
    const char *title = "ClaudeOS";
    int tw = font_text_width(&kfont_title, title);
    font_draw(s, &kfont_title, (w - tw) / 2, ly + ls + 24, title, RGB(240, 240, 245));
    int sw = font_text_width(&kfont_ui, con_status);
    gfx_fill(s, 0, ly + ls + 60, w, 20, RGB(24, 27, 42));
    gfx_gradient_v(s, 0, ly + ls + 56, w, 28, gfx_mix(RGB(18, 20, 32), RGB(30, 34, 54), (ly + ls + 56) * 255 / h),
                   gfx_mix(RGB(18, 20, 32), RGB(30, 34, 54), (ly + ls + 84) * 255 / h));
    font_draw(s, &kfont_ui, (w - sw) / 2, ly + ls + 62, con_status, RGB(160, 165, 185));
    fb_flush(s, mkrect(0, 0, w, h));
}

void bootcon_init(void) {
    if (!fb.present) return;
    con_buf = vmalloc((size_t)fb.width * fb.height * 4);
    gfx_init(&con_surf, con_buf, fb.width, fb.height, fb.width);
    con_active = true;
    strlcpy(con_status, "Starting...", sizeof(con_status));
    splash_draw();
}

void bootcon_status(const char *msg) {
    if (!con_active) return;
    strlcpy(con_status, msg, sizeof(con_status));
    splash_draw();
}

void bootcon_disable(void) {
    if (!con_active) return;
    con_active = false;
    vfree(con_buf);
    con_buf = 0;
}

/* ------------------------------------------------------------------ panic screen */

static int panic_line(surface_t *s, int x, int y, const char *text, uint32_t c) {
    font_draw(s, &kfont_mono, x, y, text, c);
    return y + kfont_mono.height;
}

void panic_screen(const char *msg, regs_t *r) {
    if (!fb.present || !fb.virt || !kfont_mono.hdr) return;
    /* draw straight into a small static buffer, line by line, to avoid allocating */
    static uint32_t line[4096 * 20];
    int w = MIN((int)fb.width, 4096);
    surface_t s;
    uint32_t bg = RGB(120, 24, 32);
    char buf[160];
    const char *lines[24];
    char store[24][160];
    int n = 0;
    lines[n++] = ":(";
    lines[n++] = "";
    lines[n++] = "ClaudeOS ran into a problem and had to stop.";
    lines[n++] = "";
    snprintf(store[n], 160, "Error: %s", msg); lines[n] = store[n]; n++;
    if (r) {
        snprintf(store[n], 160, "RIP %016lx  RSP %016lx  RFLAGS %08lx", r->rip, r->rsp, r->rflags);
        lines[n] = store[n]; n++;
        snprintf(store[n], 160, "RAX %016lx  RBX %016lx  RCX %016lx", r->rax, r->rbx, r->rcx);
        lines[n] = store[n]; n++;
        snprintf(store[n], 160, "RDX %016lx  RSI %016lx  RDI %016lx", r->rdx, r->rsi, r->rdi);
        lines[n] = store[n]; n++;
        snprintf(store[n], 160, "CR2 %016lx  ERR %lx  VECTOR %lu", read_cr2(), r->error, r->vector);
        lines[n] = store[n]; n++;
    }
    lines[n++] = "";
    lines[n++] = "Details were written to the serial port (COM1). Please restart the computer.";
    UNUSED(buf);
    int lh = kfont_mono.height;
    int top = fb.height / 5;
    for (uint32_t y = 0; y < fb.height; y += 20) {
        int bh = MIN(20, (int)fb.height - (int)y);
        gfx_init(&s, line, w, bh, w);
        gfx_fill(&s, 0, 0, w, bh, bg);
        for (int i = 1; i < n; i++) {
            int ly = top + i * (lh + 4) - (int)y;
            if (ly + lh < 0 || ly >= bh) continue;
            panic_line(&s, 60, ly, lines[i], RGB(255, 235, 235));
        }
        if (top - 40 + kfont_display.height > (int)y && top - 40 < (int)y + bh)
            font_draw(&s, &kfont_display, 60, top - 40 - (int)y, ":(", RGB(255, 255, 255));
        surface_t full = s;
        /* flush this strip */
        for (int j = 0; j < bh; j++) {
            if (fb.native32) memcpy(fb.virt + (uint64_t)(y + j) * fb.pitch, &line[j * w], w * 4);
        }
        UNUSED(full);
    }
}
