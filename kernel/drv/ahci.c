/* AHCI (SATA) driver: one command slot per port, DMA through a bounce buffer, MSI or polling */
#include <kernel.h>
#include <blk.h>
#include <pci.h>
#include <sched.h>
#include <mm.h>
#include "drivers.h"

/* HBA registers */
#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_IS   0x08
#define HBA_PI   0x0C
#define HBA_VS   0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28

/* port registers (relative to 0x100 + port * 0x80) */
#define PX_CLB  0x00
#define PX_CLBU 0x04
#define PX_FB   0x08
#define PX_FBU  0x0C
#define PX_IS   0x10
#define PX_IE   0x14
#define PX_CMD  0x18
#define PX_TFD  0x20
#define PX_SIG  0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI   0x38

#define CMD_ST  (1u << 0)
#define CMD_SUD (1u << 1)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)

#define IS_TFES (1u << 30)
#define BOUNCE_SECTORS 128               /* 64 KiB per command */

typedef struct ahci_ctrl ahci_ctrl_t;

typedef struct {
    ahci_ctrl_t *hba;
    volatile uint8_t *regs;
    int num;
    uint32_t *cmd_list;                  /* 32 headers of 32 bytes */
    uint8_t *cmd_table;                  /* slot 0 table: CFIS + PRDT */
    uint8_t *bounce;
    uint64_t bounce_phys;
    mutex_t lock;
    waitq_t wq;
    blkdev_t dev;
} ahci_port_t;

struct ahci_ctrl {
    volatile uint8_t *abar;
    bool msi;
    ahci_port_t *ports[32];
};

static int disk_count;

static inline uint32_t prd(ahci_port_t *p, int reg) { return *(volatile uint32_t *)(p->regs + reg); }
static inline void pwr(ahci_port_t *p, int reg, uint32_t v) { *(volatile uint32_t *)(p->regs + reg) = v; }
static inline uint32_t hrd(ahci_ctrl_t *h, int reg) { return *(volatile uint32_t *)(h->abar + reg); }
static inline void hwr(ahci_ctrl_t *h, int reg, uint32_t v) { *(volatile uint32_t *)(h->abar + reg) = v; }

static void ahci_irq(void *ctx) {
    ahci_ctrl_t *h = ctx;
    uint32_t is = hrd(h, HBA_IS);
    for (int i = 0; i < 32; i++) {
        if (!(is & (1u << i)) || !h->ports[i]) continue;
        ahci_port_t *p = h->ports[i];
        pwr(p, PX_IS, prd(p, PX_IS));
        wq_wake_all(&p->wq);
    }
    hwr(h, HBA_IS, is);
}

static bool wait_clear(ahci_port_t *p, int reg, uint32_t bits, int ms) {
    uint64_t end = uptime_ms() + ms;
    while (prd(p, reg) & bits) {
        if (uptime_ms() > end) return false;
        cpu_pause();
    }
    return true;
}

static void port_stop(ahci_port_t *p) {
    pwr(p, PX_CMD, prd(p, PX_CMD) & ~CMD_ST);
    wait_clear(p, PX_CMD, CMD_CR, 500);
    pwr(p, PX_CMD, prd(p, PX_CMD) & ~CMD_FRE);
    wait_clear(p, PX_CMD, CMD_FR, 500);
}

static void port_start(ahci_port_t *p) {
    wait_clear(p, PX_CMD, CMD_CR, 500);
    pwr(p, PX_CMD, prd(p, PX_CMD) | CMD_FRE | CMD_ST);
}

/* issue the command in slot 0 and wait for completion */
static int port_exec(ahci_port_t *p, uint8_t cmd, uint64_t lba, uint32_t count, uint32_t bytes, bool write) {
    uint8_t *fis = p->cmd_table;
    memset(fis, 0, 0x80);
    fis[0] = 0x27;                       /* register FIS host to device */
    fis[1] = 0x80;                       /* command */
    fis[2] = cmd;
    fis[4] = lba & 0xFF;
    fis[5] = (lba >> 8) & 0xFF;
    fis[6] = (lba >> 16) & 0xFF;
    fis[7] = 0x40;                       /* LBA mode */
    fis[8] = (lba >> 24) & 0xFF;
    fis[9] = (lba >> 32) & 0xFF;
    fis[10] = (lba >> 40) & 0xFF;
    fis[12] = count & 0xFF;
    fis[13] = (count >> 8) & 0xFF;
    uint32_t *prdt = (uint32_t *)(p->cmd_table + 0x80);
    int nprd = 0;
    if (bytes) {
        prdt[0] = (uint32_t)p->bounce_phys;
        prdt[1] = (uint32_t)(p->bounce_phys >> 32);
        prdt[2] = 0;
        prdt[3] = (bytes - 1) | (1u << 31);
        nprd = 1;
    }
    uint32_t *hdr = p->cmd_list;
    hdr[0] = 5 | (write ? (1u << 6) : 0) | ((uint32_t)nprd << 16);
    hdr[1] = 0;
    pwr(p, PX_IS, 0xFFFFFFFF);
    /* wait until the device is not busy */
    uint64_t end = uptime_ms() + 1000;
    while ((prd(p, PX_TFD) & 0x88) && uptime_ms() < end) cpu_pause();
    pwr(p, PX_CI, 1);
    end = uptime_ms() + 10000;
    for (;;) {
        if (prd(p, PX_IS) & IS_TFES) break;
        if (!(prd(p, PX_CI) & 1)) break;
        if (uptime_ms() > end) {
            klog("[ahci] port %d: command %02x timed out\n", p->num, cmd);
            return -EIO;
        }
        uint64_t f = irq_save();
        if (prd(p, PX_CI) & 1) {
            if (p->hba->msi) wq_wait_timeout(&p->wq, 20);
            else { irq_restore(f); yield(); f = irq_save(); }
        }
        irq_restore(f);
    }
    if ((prd(p, PX_IS) & IS_TFES) || (prd(p, PX_TFD) & 0x01)) {
        klog("[ahci] port %d: command %02x failed (tfd %x)\n", p->num, cmd, prd(p, PX_TFD));
        pwr(p, PX_SERR, 0xFFFFFFFF);
        port_stop(p);
        port_start(p);
        return -EIO;
    }
    return 0;
}

static int ahci_rw(blkdev_t *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    ahci_port_t *p = d->priv;
    uint8_t *b = buf;
    mutex_lock(&p->lock);
    while (count) {
        uint32_t n = MIN(count, BOUNCE_SECTORS);
        if (write) memcpy(p->bounce, b, n * 512);
        int r = port_exec(p, write ? 0x35 : 0x25, lba, n, n * 512, write);
        if (r < 0) { mutex_unlock(&p->lock); return r; }
        if (!write) memcpy(b, p->bounce, n * 512);
        b += n * 512;
        lba += n;
        count -= n;
    }
    mutex_unlock(&p->lock);
    return 0;
}

static int ahci_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf) { return ahci_rw(d, lba, count, buf, false); }
static int ahci_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf) {
    return ahci_rw(d, lba, count, (void *)buf, true);
}
static int ahci_flush(blkdev_t *d) {
    ahci_port_t *p = d->priv;
    mutex_lock(&p->lock);
    int r = port_exec(p, 0xEA, 0, 0, 0, false);   /* FLUSH CACHE EXT */
    mutex_unlock(&p->lock);
    return r;
}

static void ata_string(char *out, const uint16_t *words, int nwords) {
    for (int i = 0; i < nwords; i++) {
        out[i * 2] = words[i] >> 8;
        out[i * 2 + 1] = words[i] & 0xFF;
    }
    out[nwords * 2] = 0;
    for (int i = nwords * 2 - 1; i >= 0 && out[i] == ' '; i--) out[i] = 0;
}

static void port_init(ahci_ctrl_t *h, int num) {
    ahci_port_t *p = kzalloc(sizeof(ahci_port_t));
    p->hba = h;
    p->num = num;
    p->regs = h->abar + 0x100 + num * 0x80;
    uint32_t ssts = prd(p, PX_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) { kfree(p); return; }   /* no device / not active */
    uint32_t sig = prd(p, PX_SIG);
    if (sig != 0x00000101) {
        if (sig == 0xEB140101) klog("[ahci] port %d: ATAPI device (not supported)\n", num);
        kfree(p);
        return;
    }
    port_stop(p);
    /* command list (1 KiB), FIS area (256 B), command table (0x80 + PRDT), bounce buffer */
    uint64_t mem = pmm_alloc_contig(2, 0x100000000ULL);
    p->bounce_phys = pmm_alloc_contig(BOUNCE_SECTORS * 512 / PAGE_SIZE, 0x100000000ULL);
    if (!mem || !p->bounce_phys) { klog("[ahci] out of DMA memory\n"); return; }
    p->cmd_list = P2V(mem);
    p->cmd_table = P2V(mem + 4096);
    p->bounce = P2V(p->bounce_phys);
    uint64_t fis = mem + 1024;
    pwr(p, PX_CLB, (uint32_t)mem);
    pwr(p, PX_CLBU, (uint32_t)(mem >> 32));
    pwr(p, PX_FB, (uint32_t)fis);
    pwr(p, PX_FBU, (uint32_t)(fis >> 32));
    p->cmd_list[2] = (uint32_t)(mem + 4096);
    p->cmd_list[3] = (uint32_t)((mem + 4096) >> 32);
    pwr(p, PX_SERR, 0xFFFFFFFF);
    pwr(p, PX_IS, 0xFFFFFFFF);
    pwr(p, PX_IE, h->msi ? 0x7DC000FF : 0);      /* completion + error interrupts */
    if (hrd(h, HBA_CAP) & (1u << 27)) pwr(p, PX_CMD, prd(p, PX_CMD) | CMD_SUD);
    port_start(p);
    h->ports[num] = p;

    /* IDENTIFY DEVICE */
    if (port_exec(p, 0xEC, 0, 0, 512, false) < 0) { h->ports[num] = 0; return; }
    uint16_t *id = (uint16_t *)p->bounce;
    uint64_t sectors = *(uint64_t *)&id[100];
    if (!sectors) sectors = *(uint32_t *)&id[60];
    blkdev_t *d = &p->dev;
    snprintf(d->name, sizeof(d->name), "sd%c", 'a' + disk_count++);
    ata_string(d->model, &id[27], 20);
    d->nsectors = sectors;
    d->read = ahci_read;
    d->write = ahci_write;
    d->flush = ahci_flush;
    d->priv = p;
    klog("[ahci] %s: %s, %lu MiB (SATA port %d)\n", d->name, d->model, sectors / 2048, num);
    blk_register(d);
    blk_scan_partitions(d);
}

static void ahci_init_ctrl(pci_dev_t *pd) {
    ahci_ctrl_t *h = kzalloc(sizeof(ahci_ctrl_t));
    pci_enable_bus_master(pd);
    h->abar = pci_map_bar(pd, 5, 0);
    if (!h->abar) { klog("[ahci] cannot map ABAR\n"); kfree(h); return; }
    /* take the controller over from the firmware */
    if (hrd(h, HBA_CAP2) & 1) {
        hwr(h, HBA_BOHC, hrd(h, HBA_BOHC) | 2);
        for (int i = 0; i < 50 && (hrd(h, HBA_BOHC) & 1); i++) pit_delay_ms(1);
    }
    hwr(h, HBA_GHC, hrd(h, HBA_GHC) | (1u << 31));   /* AHCI enable */
    h->msi = pci_enable_msi(pd, ahci_irq, h);
    hwr(h, HBA_IS, 0xFFFFFFFF);
    if (h->msi) hwr(h, HBA_GHC, hrd(h, HBA_GHC) | 2);
    uint32_t vs = hrd(h, HBA_VS);
    uint32_t pi = hrd(h, HBA_PI);
    klog("[ahci] controller %04x:%04x, AHCI %x.%x, ports %x, %s\n", pd->vendor, pd->device, vs >> 16, vs & 0xFFFF,
         pi, h->msi ? "MSI" : "polling");
    for (int i = 0; i < 32; i++)
        if (pi & (1u << i)) port_init(h, i);
}

void ahci_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *d = pci_get(i);
        if (d->class_code == 0x01 && d->subclass == 0x06 && d->prog_if == 0x01) ahci_init_ctrl(d);
    }
}
