#include <kernel.h>
#include <multiboot2.h>
#include <boot.h>
#include <cpu.h>
#include <mm.h>
#include <sched.h>
#include <fb.h>
#include <vfs.h>
#include <proc.h>
#include <syscall.h>
#include <input.h>
#include <pci.h>
#include <blk.h>

void pci_register_syscalls(void);
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

static void load_initrd(void) {
    for (int i = 0; i < bootinfo.module_count; i++) {
        boot_module_t *m = &bootinfo.modules[i];
        if (strcmp(m->name, "initrd") && bootinfo.module_count > 1) continue;
        initrd_load(P2V(m->start), m->end - m->start, "");
        /* the archive is no longer needed */
        for (uint64_t p = PAGE_ALIGN_DOWN(m->start); p < PAGE_ALIGN_UP(m->end); p += PAGE_SIZE) pmm_free(p);
        return;
    }
    klog("[init] warning: no initrd module found\n");
}

static int init_thread(void *arg) {
    UNUSED(arg);
    cpu_measure_mhz();
    rtc_init();
    bootcon_status("Mounting file systems...");
    vfs_init();
    load_initrd();
    devfs_init();
    extern void pty_init(void);
    pty_init();
    vfs_mkdir("/tmp");
    vfs_mkdir("/home");
    syscall_init();
    bootcon_status("Detecting hardware...");
    pci_init();
    pci_register_syscalls();
    ata_init();
    storage_init();
    storage_register_syscalls();
    extern void audio_init(void);
    audio_init();
    extern void net_init(void);
    net_init();

    if (cmdline_has("usertest")) {
        char *argv[] = { "hello", 0 };
        char *envp[] = { "PATH=/bin", "HOME=/home", 0 };
        int pid = proc_spawn("/bin/hello", argv, envp, 0, "/", current->pid, 0);
        int status = -1;
        if (pid > 0) proc_waitpid(pid, &status, 0);
        klog("[init] usertest pid %d exited with status %d\n", pid, status);
    }
    bootcon_status("Starting input devices...");
    ps2_init();
    bootcon_status("Loading desktop...");
    extern void wm_init(void);
    wm_init();
    if (cmdline_has("testpanic")) *(volatile uint64_t *)0xdead0000 = 1;
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
    kfonts_init();
    fb_init();
    bootcon_init();

    sched_init();
    pic_init();
    pit_init(1000);

    kthread_create("init", init_thread, 0);
    sched_idle_loop();
}
