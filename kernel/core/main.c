#include <kernel.h>
#include <multiboot2.h>
#include <boot.h>
#include <cpu.h>
#include <mm.h>
#include <sched.h>
#include "drivers.h"

bootinfo_t bootinfo;

static void parse_multiboot(uint64_t mbi_phys) {
    mb2_info_t *info = P2V(mbi_phys);
    bootinfo.mbi_phys = mbi_phys;
    bootinfo.mbi_size = info->total_size;
    for (mb2_tag_t *tag = (mb2_tag_t *)(info + 1); tag->type != MB2_TAG_END;
         tag = (mb2_tag_t *)((uint8_t *)tag + ((tag->size + 7) & ~7u))) {
        switch (tag->type) {
        case MB2_TAG_CMDLINE:
            strlcpy(bootinfo.cmdline, ((mb2_tag_string_t *)tag)->string, sizeof(bootinfo.cmdline));
            break;
        case MB2_TAG_BOOTLOADER:
            strlcpy(bootinfo.loader, ((mb2_tag_string_t *)tag)->string, sizeof(bootinfo.loader));
            break;
        case MB2_TAG_MODULE: {
            mb2_tag_module_t *m = (mb2_tag_module_t *)tag;
            if (bootinfo.module_count < BOOT_MAX_MODULES) {
                boot_module_t *bm = &bootinfo.modules[bootinfo.module_count++];
                bm->start = m->mod_start;
                bm->end = m->mod_end;
                strlcpy(bm->name, m->cmdline, sizeof(bm->name));
            }
            break;
        }
        case MB2_TAG_MMAP: {
            mb2_tag_mmap_t *mm = (mb2_tag_mmap_t *)tag;
            uint8_t *p = (uint8_t *)mm->entries;
            uint8_t *end = (uint8_t *)tag + tag->size;
            while (p < end && bootinfo.mmap_count < BOOT_MAX_MMAP) {
                mb2_mmap_entry_t *e = (mb2_mmap_entry_t *)p;
                bootinfo.mmap[bootinfo.mmap_count++] = (boot_mmap_t){ e->addr, e->len, e->type };
                p += mm->entry_size;
            }
            break;
        }
        case MB2_TAG_FRAMEBUFFER: {
            mb2_tag_fb_t *fb = (mb2_tag_fb_t *)tag;
            bootinfo.fb_addr = fb->addr;
            bootinfo.fb_width = fb->width;
            bootinfo.fb_height = fb->height;
            bootinfo.fb_pitch = fb->pitch;
            bootinfo.fb_bpp = fb->bpp;
            bootinfo.fb_type = fb->fb_type;
            if (fb->fb_type == 1) {
                bootinfo.fb_rpos = fb->red_pos; bootinfo.fb_rsize = fb->red_size;
                bootinfo.fb_gpos = fb->green_pos; bootinfo.fb_gsize = fb->green_size;
                bootinfo.fb_bpos = fb->blue_pos; bootinfo.fb_bsize = fb->blue_size;
            }
            break;
        }
        case MB2_TAG_ACPI_OLD:
        case MB2_TAG_ACPI_NEW: {
            mb2_tag_acpi_t *a = (mb2_tag_acpi_t *)tag;
            size_t n = MIN(tag->size - 8, sizeof(bootinfo.rsdp));
            if (tag->type == MB2_TAG_ACPI_NEW || !bootinfo.has_rsdp) {
                memcpy(bootinfo.rsdp, a->rsdp, n);
                bootinfo.has_rsdp = true;
            }
            break;
        }
        }
    }
}

bool cmdline_has(const char *opt) {
    size_t n = strlen(opt);
    const char *p = bootinfo.cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!strncmp(p, opt, n) && (p[n] == 0 || p[n] == ' ' || p[n] == '=')) return true;
        while (*p && *p != ' ') p++;
    }
    return false;
}

const char *cmdline_get(const char *key) {
    static char val[64];
    size_t n = strlen(key);
    const char *p = bootinfo.cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!strncmp(p, key, n) && p[n] == '=') {
            p += n + 1;
            size_t i = 0;
            while (*p && *p != ' ' && i < sizeof(val) - 1) val[i++] = *p++;
            val[i] = 0;
            return val;
        }
        while (*p && *p != ' ') p++;
    }
    return 0;
}

static void heap_selftest(void) {
    void *ptrs[64];
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 64; i++) {
            size_t sz = (i * 37 + round * 101) % 5000 + 1;
            ptrs[i] = kmalloc(sz);
            assert(ptrs[i]);
            memset(ptrs[i], i, sz);
        }
        for (int i = 0; i < 64; i += 2) kfree(ptrs[i]);
        for (int i = 1; i < 64; i += 2) {
            size_t sz = (i * 37 + round * 101) % 5000 + 1;
            uint8_t *p = ptrs[i];
            for (size_t j = 0; j < sz; j++) if (p[j] != (uint8_t)i) panic("heap selftest corrupted");
            kfree(p);
        }
    }
    void *big = vmalloc(8 << 20);
    memset(big, 0xAB, 8 << 20);
    vfree(big);
    klog("[mm] heap self-test passed (heap in use: %lu bytes)\n", (uint64_t)heap_used_bytes());
}

static volatile int counter_a, counter_b;
static mutex_t test_mutex;
static waitq_t test_wq;

static int test_worker(void *arg) {
    volatile int *c = arg;
    for (int i = 0; i < 50; i++) {
        mutex_lock(&test_mutex);
        (*c)++;
        mutex_unlock(&test_mutex);
        if (i % 10 == 0) sleep_ms(2);
    }
    uint64_t f = irq_save();
    wq_wake_all(&test_wq);
    irq_restore(f);
    return 0;
}

static int init_thread(void *arg) {
    UNUSED(arg);
    klog("[init] scheduler running, spawning test threads\n");
    kthread_create("test-a", test_worker, (void *)&counter_a);
    kthread_create("test-b", test_worker, (void *)&counter_b);
    uint64_t f = irq_save();
    while (counter_a < 50 || counter_b < 50) wq_wait_timeout(&test_wq, 100);
    irq_restore(f);
    uint64_t t0 = uptime_ms();
    sleep_ms(50);
    klog("[init] threads done (a=%d b=%d), slept %lu ms\n", counter_a, counter_b, uptime_ms() - t0);
    klog("[boot] done\n");
    return 0;
}

void kmain(uint32_t magic, uint64_t mbi_phys) {
    serial_init();
    kprintf("\n[boot] ClaudeOS kernel starting\n");
    if (magic != MB2_BOOTLOADER_MAGIC) panic("not booted by a multiboot2 loader (magic %x)", magic);
    parse_multiboot(mbi_phys);
    kprintf("[boot] loader: %s, cmdline: '%s'\n", bootinfo.loader, bootinfo.cmdline);

    gdt_init();
    idt_init();
    cpu_init_features();
    pmm_init();
    vmm_init();
    heap_selftest();

    sched_init();
    pic_init();
    pit_init(1000);

    kthread_create("init", init_thread, 0);
    sched_idle_loop();
}
