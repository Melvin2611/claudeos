/* ACPI: find the FADT, soft-off (S5) and reset */
#include <kernel.h>
#include <boot.h>
#include <mm.h>
#include <smp.h>
#include "drivers.h"

/* MADT results */
uint32_t acpi_cpu_apic_ids[MAX_CPUS];
int acpi_cpu_count;
uint64_t acpi_lapic_phys = 0xFEE00000;

static void parse_madt(const uint8_t *m, uint32_t len) {
    acpi_lapic_phys = *(const uint32_t *)(m + 36);
    for (uint32_t off = 44; off + 2 <= len;) {
        uint8_t type = m[off], l = m[off + 1];
        if (l < 2) break;
        uint32_t id = 0xFFFFFFFF, flags = 0;
        if (type == 0 && l >= 8) {                /* processor local APIC */
            id = m[off + 3];
            flags = *(const uint32_t *)(m + off + 4);
        } else if (type == 9 && l >= 16) {        /* processor local x2APIC */
            id = *(const uint32_t *)(m + off + 4);
            flags = *(const uint32_t *)(m + off + 8);
        } else if (type == 5 && l >= 12) {        /* 64-bit LAPIC address override */
            acpi_lapic_phys = *(const uint64_t *)(m + off + 4);
        }
        if (id != 0xFFFFFFFF && (flags & 1) && acpi_cpu_count < MAX_CPUS) {
            bool dup = false;
            for (int i = 0; i < acpi_cpu_count; i++) if (acpi_cpu_apic_ids[i] == id) dup = true;
            if (!dup) acpi_cpu_apic_ids[acpi_cpu_count++] = id;
        }
        off += l;
    }
}

void fs_sync_all(void);

static struct {
    bool ok;
    uint32_t pm1a_cnt, pm1b_cnt, smi_cmd;
    uint8_t acpi_enable;
    uint16_t slp_typa, slp_typb;
    bool s5_found;
    bool reset_ok;
    uint8_t reset_space, reset_value;
    uint64_t reset_addr;
} acpi;

static void *map_table(uint64_t phys, uint32_t *len_out) {
    uint8_t *h = ioremap(phys, 36, CACHE_WB);
    if (!h) return 0;
    uint32_t len = *(uint32_t *)(h + 4);
    iounmap(h);
    if (len < 36 || len > 4 * 1024 * 1024) return 0;
    if (len_out) *len_out = len;
    return ioremap(phys, len, CACHE_WB);
}

static bool checksum_ok(const uint8_t *p, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s == 0;
}

static const uint8_t *find_rsdp(void) {
    if (bootinfo.has_rsdp) return bootinfo.rsdp;
    /* BIOS area scan */
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
        const uint8_t *p = P2V(a);
        if (!memcmp(p, "RSD PTR ", 8) && checksum_ok(p, 20)) return p;
    }
    return 0;
}

static void parse_s5(const uint8_t *dsdt, uint32_t len) {
    for (uint32_t i = 36; i + 8 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4)) continue;
        /* NameOp (0x08) precedes the name, possibly with a root/parent prefix */
        if (!(dsdt[i - 1] == 0x08 || (dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\'))) continue;
        const uint8_t *p = dsdt + i + 4;
        if (*p != 0x12) continue;            /* PackageOp */
        p++;
        p += ((*p & 0xC0) >> 6) + 1;          /* PkgLength */
        p++;                                  /* NumElements */
        uint16_t vals[2];
        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A) { vals[k] = p[1]; p += 2; }
            else if (*p == 0x00 || *p == 0x01) { vals[k] = *p; p++; }
            else { vals[k] = *p; p++; }
        }
        acpi.slp_typa = vals[0];
        acpi.slp_typb = vals[1];
        acpi.s5_found = true;
        return;
    }
}

void acpi_init(void) {
    const uint8_t *rsdp = find_rsdp();
    if (!rsdp) { klog("[acpi] no RSDP found\n"); return; }
    uint8_t rev = rsdp[15];
    uint64_t xsdt = rev >= 2 ? *(const uint64_t *)(rsdp + 24) : 0;
    uint64_t rsdt = *(const uint32_t *)(rsdp + 16);
    uint32_t len;
    uint8_t *root = map_table(xsdt ? xsdt : rsdt, &len);
    if (!root) return;
    int entry = xsdt ? 8 : 4;
    uint64_t fadt_phys = 0;
    for (uint32_t off = 36; off + entry <= len; off += entry) {
        uint64_t a = entry == 8 ? *(uint64_t *)(root + off) : *(uint32_t *)(root + off);
        uint8_t *h = map_table(a, 0);
        if (!h) continue;
        bool is_fadt = !memcmp(h, "FACP", 4), is_madt = !memcmp(h, "APIC", 4);
        iounmap(h);
        if (is_fadt) fadt_phys = a;
        if (is_madt) {
            uint32_t mlen;
            uint8_t *m = map_table(a, &mlen);
            if (m) { parse_madt(m, mlen); iounmap(m); }
        }
    }
    iounmap(root);
    if (!fadt_phys) { klog("[acpi] no FADT\n"); return; }
    uint32_t flen;
    uint8_t *fadt = map_table(fadt_phys, &flen);
    acpi.smi_cmd = *(uint32_t *)(fadt + 48);
    acpi.acpi_enable = fadt[52];
    acpi.pm1a_cnt = *(uint32_t *)(fadt + 64);
    acpi.pm1b_cnt = *(uint32_t *)(fadt + 68);
    uint64_t dsdt = *(uint32_t *)(fadt + 40);
    if (flen >= 148 && *(uint64_t *)(fadt + 140)) dsdt = *(uint64_t *)(fadt + 140);
    if (flen >= 129) {
        uint32_t flags = *(uint32_t *)(fadt + 112);
        if (flags & (1 << 10)) {
            acpi.reset_ok = true;
            acpi.reset_space = fadt[116];
            acpi.reset_addr = *(uint64_t *)(fadt + 120);
            acpi.reset_value = fadt[128];
        }
    }
    iounmap(fadt);
    uint32_t dlen;
    uint8_t *d = map_table(dsdt, &dlen);
    if (d) { parse_s5(d, dlen); iounmap(d); }
    acpi.ok = true;
    klog("[acpi] %d CPU(s) in MADT, local APIC at %lx\n", acpi_cpu_count, acpi_lapic_phys);
    klog("[acpi] PM1a_CNT %x, S5 %s (typ %u), reset register %s\n", acpi.pm1a_cnt, acpi.s5_found ? "found" : "missing",
         acpi.slp_typa, acpi.reset_ok ? "yes" : "no");
}

NORETURN void power_off(void) {
    fs_sync_all();
    klog("[acpi] powering off\n");
    cli();
    if (acpi.ok && acpi.pm1a_cnt && acpi.s5_found) {
        if (acpi.smi_cmd && acpi.acpi_enable && !(inw(acpi.pm1a_cnt) & 1)) {
            outb(acpi.smi_cmd, acpi.acpi_enable);
            for (int i = 0; i < 300 && !(inw(acpi.pm1a_cnt) & 1); i++) pit_delay_ms(1);
        }
        outw(acpi.pm1a_cnt, (uint16_t)((acpi.slp_typa << 10) | (1 << 13)));
        if (acpi.pm1b_cnt) outw(acpi.pm1b_cnt, (uint16_t)((acpi.slp_typb << 10) | (1 << 13)));
        pit_delay_ms(50);
    }
    /* emulator fallbacks: QEMU (new/old), VirtualBox */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    klog("[acpi] power off failed - it is now safe to turn off the computer\n");
    for (;;) hlt();
}

NORETURN void power_reboot(void) {
    fs_sync_all();
    klog("[acpi] rebooting\n");
    cli();
    if (acpi.reset_ok && acpi.reset_space == 1) outb((uint16_t)acpi.reset_addr, acpi.reset_value);
    pit_delay_ms(50);
    for (int i = 0; i < 10000 && (inb(0x64) & 2); i++) io_wait();
    outb(0x64, 0xFE);                    /* keyboard controller reset line */
    pit_delay_ms(50);
    outb(0xCF9, 0x06);                   /* PCI reset control */
    pit_delay_ms(50);
    /* triple fault */
    struct { uint16_t l; uint64_t b; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(null_idt));
    for (;;) hlt();
}
