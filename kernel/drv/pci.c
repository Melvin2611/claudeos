/* PCI configuration space access (mechanism #1) and bus enumeration */
#include <kernel.h>
#include <pci.h>
#include <syscall.h>
#include <mm.h>
#include <smp.h>

#define MAX_PCI 256
static pci_dev_t devs[MAX_PCI];
static int ndevs;

static uint32_t cfg_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    return inl(0xCFC);
}

static void cfg_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

uint32_t pci_read32(pci_dev_t *d, uint8_t off) { return cfg_read(d->bus, d->dev, d->func, off); }
void pci_write32(pci_dev_t *d, uint8_t off, uint32_t v) { cfg_write(d->bus, d->dev, d->func, off, v); }
uint16_t pci_read16(pci_dev_t *d, uint8_t off) { return (uint16_t)(pci_read32(d, off & ~3) >> ((off & 2) * 8)); }
void pci_write16(pci_dev_t *d, uint8_t off, uint16_t v) {
    uint32_t old = pci_read32(d, off & ~3);
    int sh = (off & 2) * 8;
    old = (old & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_write32(d, off & ~3, old);
}

void pci_enable_bus_master(pci_dev_t *d) {
    uint16_t cmd = pci_read16(d, 0x04);
    cmd |= 0x07;   /* I/O space, memory space, bus master */
    cmd &= ~0x400; /* interrupts enabled */
    pci_write16(d, 0x04, cmd);
}

static void add_dev(uint8_t bus, uint8_t dev, uint8_t func) {
    if (ndevs >= MAX_PCI) return;
    uint32_t id = cfg_read(bus, dev, func, 0);
    pci_dev_t *d = &devs[ndevs++];
    d->bus = bus;
    d->dev = dev;
    d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    uint32_t cls = cfg_read(bus, dev, func, 8);
    d->class_code = cls >> 24;
    d->subclass = (cls >> 16) & 0xFF;
    d->prog_if = (cls >> 8) & 0xFF;
    d->irq = cfg_read(bus, dev, func, 0x3C) & 0xFF;
    for (int i = 0; i < 6; i++) d->bar[i] = cfg_read(bus, dev, func, 0x10 + i * 4);
}

void pci_init(void) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = cfg_read(bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = (cfg_read(bus, dev, 0, 0x0C) >> 16) & 0xFF;
            int nfunc = (hdr & 0x80) ? 8 : 1;
            for (int f = 0; f < nfunc; f++) {
                if ((cfg_read(bus, dev, f, 0) & 0xFFFF) == 0xFFFF) continue;
                add_dev(bus, dev, f);
            }
        }
    }
    klog("[pci] %d devices\n", ndevs);
    for (int i = 0; i < ndevs; i++) {
        pci_dev_t *d = &devs[i];
        klog("[pci]   %02x:%02x.%x %04x:%04x class %02x.%02x irq %u  %s\n", d->bus, d->dev, d->func, d->vendor,
             d->device, d->class_code, d->subclass, d->irq, pci_class_name(d->class_code, d->subclass));
    }
}

int pci_count(void) { return ndevs; }
pci_dev_t *pci_get(int i) { return i >= 0 && i < ndevs ? &devs[i] : 0; }

pci_dev_t *pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device) return &devs[i];
    return 0;
}

pci_dev_t *pci_find_class(uint8_t cls, uint8_t sub) {
    for (int i = 0; i < ndevs; i++)
        if (devs[i].class_code == cls && devs[i].subclass == sub) return &devs[i];
    return 0;
}

const char *pci_class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
    case 0x01:
        if (sub == 0x01) return "IDE controller";
        if (sub == 0x06) return "SATA controller (AHCI)";
        if (sub == 0x08) return "NVMe controller";
        return "Storage controller";
    case 0x02: return "Ethernet controller";
    case 0x03: return "VGA compatible controller";
    case 0x04: return sub == 0x01 ? "Audio device (AC'97)" : sub == 0x03 ? "Audio device (HD Audio)" : "Multimedia device";
    case 0x06:
        if (sub == 0x00) return "Host bridge";
        if (sub == 0x01) return "ISA bridge";
        if (sub == 0x04) return "PCI bridge";
        if (sub == 0x80) return "System bridge";
        return "Bridge";
    case 0x0C: return sub == 0x03 ? "USB controller" : "Serial bus controller";
    default: return "Device";
    }
}

SYSCALL_DEF(sys_pci_info) {
    SYSCALL_UNUSED_ARGS;
    pci_dev_t *d = pci_get((int)a1);
    if (!d) return 0;
    kpciinfo_t info;
    memset(&info, 0, sizeof(info));
    info.bus = d->bus;
    info.dev = d->dev;
    info.func = d->func;
    info.class_code = d->class_code;
    info.subclass = d->subclass;
    info.prog_if = d->prog_if;
    info.irq = d->irq;
    info.vendor = d->vendor;
    info.device = d->device;
    strlcpy(info.description, pci_class_name(d->class_code, d->subclass), sizeof(info.description));
    if (copy_to_user((void *)a2, &info, sizeof(info)) < 0) return -EFAULT;
    return 1;
}

/* ------------------------------------------------------------------ BARs, capabilities, MSI */

uint64_t pci_bar_phys(pci_dev_t *d, int i) {
    uint32_t b = d->bar[i];
    if (b & 1) return b & ~3u;
    uint64_t a = b & ~0xFULL;
    if (((b >> 1) & 3) == 2 && i < 5) a |= (uint64_t)d->bar[i + 1] << 32;
    return a;
}

uint64_t pci_bar_size(pci_dev_t *d, int i) {
    uint8_t off = 0x10 + i * 4;
    uint16_t cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, cmd & ~3);                   /* no decoding while probing */
    uint32_t orig = pci_read32(d, off);
    pci_write32(d, off, 0xFFFFFFFF);
    uint32_t v = pci_read32(d, off);
    pci_write32(d, off, orig);
    uint64_t size;
    if (orig & 1) {
        size = (uint16_t)(~(v & ~3u) + 1);
    } else if (((orig >> 1) & 3) == 2 && i < 5) {
        uint32_t orig_hi = pci_read32(d, off + 4);
        pci_write32(d, off + 4, 0xFFFFFFFF);
        uint32_t vh = pci_read32(d, off + 4);
        pci_write32(d, off + 4, orig_hi);
        uint64_t mask = ((uint64_t)vh << 32) | (v & ~0xFu);
        size = ~mask + 1;
    } else {
        size = (uint32_t)(~(v & ~0xFu) + 1);
    }
    pci_write16(d, 0x04, cmd);
    return size;
}

void *pci_map_bar(pci_dev_t *d, int i, uint64_t *size_out) {
    uint64_t phys = pci_bar_phys(d, i), size = pci_bar_size(d, i);
    if (!phys || !size || (d->bar[i] & 1)) return 0;
    if (size > (64ULL << 20)) size = 64ULL << 20;
    if (size_out) *size_out = size;
    return ioremap(phys, size, CACHE_UC);
}

uint8_t pci_find_cap(pci_dev_t *d, uint8_t id) {
    if (!(pci_read16(d, 0x06) & 0x10)) return 0;
    uint8_t p = pci_read32(d, 0x34) & 0xFC;
    for (int n = 0; p && n < 48; n++) {
        uint32_t v = pci_read32(d, p);
        if ((v & 0xFF) == id) return p;
        p = (v >> 8) & 0xFC;
    }
    return 0;
}

/* route the device's interrupt to a local APIC vector: MSI if available, else MSI-X entry 0 */
bool pci_enable_msi(pci_dev_t *d, void (*fn)(void *), void *ctx) {
    uint8_t vec;
    uint8_t cap = pci_find_cap(d, 0x05);
    uint8_t capx = pci_find_cap(d, 0x11);
    if ((!cap && !capx) || !msi_alloc_vector(fn, ctx, &vec)) return false;
    uint32_t addr = 0xFEE00000u | (lapic_bsp_id << 12);
    if (cap) {
        uint16_t ctrl = pci_read16(d, cap + 2);
        bool is64 = ctrl & (1 << 7);
        pci_write32(d, cap + 4, addr);
        if (is64) {
            pci_write32(d, cap + 8, 0);
            pci_write16(d, cap + 12, vec);
        } else {
            pci_write16(d, cap + 8, vec);
        }
        ctrl &= ~(7 << 4);                            /* one message */
        pci_write16(d, cap + 2, ctrl | 1);
    } else {
        uint16_t ctrl = pci_read16(d, capx + 2);
        uint32_t tbl = pci_read32(d, capx + 4);
        int bir = tbl & 7;
        uint64_t phys = pci_bar_phys(d, bir) + (tbl & ~7u);
        volatile uint32_t *entry = ioremap(phys, 16, CACHE_UC);
        if (!entry) return false;
        pci_write16(d, capx + 2, ctrl | (1 << 15) | (1 << 14));   /* enable, masked while programming */
        entry[0] = addr;
        entry[1] = 0;
        entry[2] = vec;
        entry[3] = 0;                                 /* unmask entry 0 */
        pci_write16(d, capx + 2, (ctrl | (1 << 15)) & ~(1 << 14));
    }
    pci_write16(d, 0x04, pci_read16(d, 0x04) | 0x400);   /* disable legacy INTx */
    return true;
}

/* ------------------------------------------------------------------ graphics adapter names */

static const struct { uint16_t dev; const char *name; } amd_gpus[] = {
    { 0x744C, "Radeon RX 7900 XT/XTX" }, { 0x7448, "Radeon PRO W7900" }, { 0x747E, "Radeon RX 7800 XT / 7700 XT" },
    { 0x7480, "Radeon RX 7600 / 7600 XT" }, { 0x7550, "Radeon RX 9070 / 9070 XT" }, { 0x7590, "Radeon RX 9060 XT" },
    { 0x73BF, "Radeon RX 6800/6900 XT" }, { 0x73AF, "Radeon RX 6900 XT" }, { 0x73A5, "Radeon RX 6950 XT" },
    { 0x73DF, "Radeon RX 6700/6750 XT" }, { 0x73FF, "Radeon RX 6600/6650 XT" }, { 0x743F, "Radeon RX 6400/6500 XT" },
    { 0x731F, "Radeon RX 5600/5700 XT" }, { 0x7340, "Radeon RX 5500 XT" }, { 0x66AF, "Radeon VII" },
    { 0x687F, "Radeon RX Vega 56/64" }, { 0x67DF, "Radeon RX 470/480/570/580" }, { 0x67FF, "Radeon RX 550/560" },
    { 0x699F, "Radeon RX 550 / 540" }, { 0x6FDF, "Radeon RX 580 2048SP" },
    { 0x164E, "Radeon Graphics (Raphael, Ryzen 7000)" }, { 0x13C0, "Radeon Graphics (Granite Ridge, Ryzen 9000)" },
    { 0x15BF, "Radeon 780M (Phoenix)" }, { 0x15C8, "Radeon 740M (Phoenix)" }, { 0x1900, "Radeon 780M (Hawk Point)" },
    { 0x150E, "Radeon 890M (Strix Point)" }, { 0x1586, "Radeon 8060S (Strix Halo)" },
    { 0x1681, "Radeon 680M (Rembrandt)" }, { 0x1638, "Radeon Vega (Cezanne)" }, { 0x1636, "Radeon Vega (Renoir)" },
    { 0x15D8, "Radeon Vega (Picasso)" }, { 0x15DD, "Radeon Vega (Raven Ridge)" }, { 0x163F, "Radeon (Van Gogh, Steam Deck)" },
    { 0x1435, "Radeon (Aerith, Steam Deck)" },
};

/* describe the display adapter (the desktop uses the firmware framebuffer on every GPU) */
void pci_gpu_name(char *out, size_t n) {
    pci_dev_t *g = 0;
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].class_code != 0x03) continue;
        if (!g || devs[i].vendor == 0x1002) g = &devs[i];   /* prefer the AMD card */
    }
    if (!g) { strlcpy(out, "Framebuffer", n); return; }
    if (g->vendor == 0x1002) {
        for (size_t i = 0; i < ARRAY_SIZE(amd_gpus); i++)
            if (amd_gpus[i].dev == g->device) { snprintf(out, n, "AMD %s", amd_gpus[i].name); return; }
        snprintf(out, n, "AMD Radeon (device %04x)", g->device);
    } else if (g->vendor == 0x1234 && g->device == 0x1111) {
        strlcpy(out, "Bochs/QEMU standard VGA", n);
    } else if (g->vendor == 0x1AF4) {
        strlcpy(out, "Virtio GPU", n);
    } else if (g->vendor == 0x15AD) {
        strlcpy(out, "VMware SVGA II", n);
    } else if (g->vendor == 0x80EE) {
        strlcpy(out, "VirtualBox Graphics Adapter", n);
    } else if (g->vendor == 0x10DE) {
        snprintf(out, n, "NVIDIA GPU (device %04x)", g->device);
    } else if (g->vendor == 0x8086) {
        snprintf(out, n, "Intel Graphics (device %04x)", g->device);
    } else {
        snprintf(out, n, "Display adapter %04x:%04x", g->vendor, g->device);
    }
}

void pci_register_syscalls(void) { syscall_register(SYS_PCI_INFO, sys_pci_info); }
