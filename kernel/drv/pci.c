/* PCI configuration space access (mechanism #1) and bus enumeration */
#include <kernel.h>
#include <pci.h>
#include <syscall.h>
#include <mm.h>

#define MAX_PCI 64
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

void pci_register_syscalls(void) { syscall_register(SYS_PCI_INFO, sys_pci_info); }
