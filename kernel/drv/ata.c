/* ATA PIO driver (legacy IDE, LBA28/LBA48, polling) */
#include <kernel.h>
#include <blk.h>
#include <pci.h>
#include <sched.h>
#include <mm.h>
#include <boot.h>

#define ATA_DATA 0
#define ATA_ERR 1
#define ATA_COUNT 2
#define ATA_LBA0 3
#define ATA_LBA1 4
#define ATA_LBA2 5
#define ATA_DRIVE 6
#define ATA_STATUS 7
#define ATA_CMD 7

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF 0x20
#define ST_BSY 0x80

typedef struct {
    uint16_t io, ctrl;
    int slave;
    bool lba48;
    blkdev_t dev;
    mutex_t *lock;
    uint16_t bm;            /* bus master registers (0 = PIO only) */
    uint32_t *prd;          /* physical region descriptor table */
    uint64_t prd_phys;
    uint8_t *dma_buf;       /* 64 KiB bounce buffer below 4 GiB */
    uint64_t dma_phys;
    bool dma_failed;
} ata_disk_t;

static mutex_t channel_lock[2];
static uint16_t bm_base[2];
#define DMA_SECTORS 128

static void delay400(uint16_t ctrl) { for (int i = 0; i < 4; i++) inb(ctrl); }

static int wait_not_busy(uint16_t io, uint32_t timeout) {
    for (uint32_t i = 0; i < timeout; i++) {
        uint8_t st = inb(io + ATA_STATUS);
        if (!(st & ST_BSY)) return st;
        if (i > 1000) cpu_pause();
    }
    return -1;
}

static int wait_drq(uint16_t io) {
    for (uint32_t i = 0; i < 2000000; i++) {
        uint8_t st = inb(io + ATA_STATUS);
        if (st & ST_BSY) continue;
        if (st & (ST_ERR | ST_DF)) return -1;
        if (st & ST_DRQ) return 0;
    }
    return -1;
}

static void select_lba(ata_disk_t *a, uint64_t lba, uint32_t count) {
    uint16_t io = a->io;
    if (a->lba48) {
        outb(io + ATA_DRIVE, 0x40 | (a->slave << 4));
        delay400(a->ctrl);
        outb(io + ATA_COUNT, (count >> 8) & 0xFF);
        outb(io + ATA_LBA0, (lba >> 24) & 0xFF);
        outb(io + ATA_LBA1, (lba >> 32) & 0xFF);
        outb(io + ATA_LBA2, (lba >> 40) & 0xFF);
        outb(io + ATA_COUNT, count & 0xFF);
        outb(io + ATA_LBA0, lba & 0xFF);
        outb(io + ATA_LBA1, (lba >> 8) & 0xFF);
        outb(io + ATA_LBA2, (lba >> 16) & 0xFF);
    } else {
        outb(io + ATA_DRIVE, 0xE0 | (a->slave << 4) | ((lba >> 24) & 0x0F));
        delay400(a->ctrl);
        outb(io + ATA_COUNT, count & 0xFF);
        outb(io + ATA_LBA0, lba & 0xFF);
        outb(io + ATA_LBA1, (lba >> 8) & 0xFF);
        outb(io + ATA_LBA2, (lba >> 16) & 0xFF);
    }
}

static int ata_rw(blkdev_t *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    ata_disk_t *a = d->priv;
    uint8_t *p = buf;
    mutex_lock(a->lock);
    while (count) {
        uint32_t n = MIN(count, a->lba48 ? 256u : 256u);
        if (wait_not_busy(a->io, 5000000) < 0) { mutex_unlock(a->lock); return -EIO; }
        select_lba(a, lba, n == 256 ? 0 : n);
        uint8_t cmd = write ? (a->lba48 ? 0x34 : 0x30) : (a->lba48 ? 0x24 : 0x20);
        outb(a->io + ATA_CMD, cmd);
        for (uint32_t s = 0; s < n; s++) {
            delay400(a->ctrl);
            if (wait_drq(a->io) < 0) {
                klog("[ata] %s error at lba %lu (status %x err %x)\n", d->name, lba + s, inb(a->io + ATA_STATUS),
                     inb(a->io + ATA_ERR));
                mutex_unlock(a->lock);
                return -EIO;
            }
            if (write) outsw(a->io + ATA_DATA, p, 256);
            else insw(a->io + ATA_DATA, p, 256);
            p += 512;
        }
        if (write) {
            if (wait_not_busy(a->io, 5000000) < 0) { mutex_unlock(a->lock); return -EIO; }
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(a->lock);
    return 0;
}

/* Bus master DMA transfer of up to DMA_SECTORS sectors through the bounce buffer */
static int ata_dma_chunk(ata_disk_t *a, uint64_t lba, uint32_t n, bool write) {
    uint16_t bm = a->bm;
    a->prd[0] = (uint32_t)a->dma_phys;
    a->prd[1] = (n * 512 == 65536 ? 0 : n * 512) | 0x80000000u;
    outb(bm + 0, 0);                                  /* stop */
    outl(bm + 4, (uint32_t)a->prd_phys);
    outb(bm + 2, inb(bm + 2) | 0x06);                 /* clear interrupt + error */
    outb(bm + 0, write ? 0x00 : 0x08);                /* direction: 8 = device -> memory */
    if (wait_not_busy(a->io, 5000000) < 0) return -EIO;
    select_lba(a, lba, n == 256 ? 0 : n);
    outb(a->io + ATA_CMD, write ? (a->lba48 ? 0x35 : 0xCA) : (a->lba48 ? 0x25 : 0xC8));
    outb(bm + 0, (write ? 0x00 : 0x08) | 0x01);       /* start */
    uint32_t spins = 0;
    for (;;) {
        uint8_t bs = inb(bm + 2);
        if (bs & 0x02) break;                         /* error */
        if (bs & 0x04 || !(bs & 0x01)) break;          /* interrupt or no longer active */
        if (++spins > 200000000) break;
        if (spins > 100) cpu_pause();
        if (spins > 20000 && ints_enabled()) yield();
    }
    outb(bm + 0, 0);
    uint8_t bs = inb(bm + 2);
    outb(bm + 2, bs | 0x06);
    int st = wait_not_busy(a->io, 5000000);
    if (st < 0 || (st & (ST_ERR | ST_DF)) || (bs & 0x02)) return -EIO;
    return 0;
}

static int ata_dma_rw(blkdev_t *d, uint64_t lba, uint32_t count, uint8_t *p, bool write) {
    ata_disk_t *a = d->priv;
    mutex_lock(a->lock);
    int r = 0;
    while (count && r == 0) {
        uint32_t n = MIN(count, DMA_SECTORS);
        if (write) memcpy(a->dma_buf, p, n * 512);
        r = ata_dma_chunk(a, lba, n, write);
        if (r == 0 && !write) memcpy(p, a->dma_buf, n * 512);
        lba += n;
        count -= n;
        p += n * 512;
    }
    mutex_unlock(a->lock);
    if (r < 0) {
        klog("[ata] %s: DMA error, falling back to PIO\n", d->name);
        a->dma_failed = true;
    }
    return r;
}

static int ata_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf) {
    ata_disk_t *a = d->priv;
    if (a->bm && !a->dma_failed && ata_dma_rw(d, lba, count, buf, false) == 0) return 0;
    return ata_rw(d, lba, count, buf, false);
}

static int ata_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf) {
    ata_disk_t *a = d->priv;
    if (a->bm && !a->dma_failed && ata_dma_rw(d, lba, count, (uint8_t *)buf, true) == 0) return 0;
    return ata_rw(d, lba, count, (void *)buf, true);
}

static int ata_flush(blkdev_t *d) {
    ata_disk_t *a = d->priv;
    mutex_lock(a->lock);
    wait_not_busy(a->io, 5000000);
    outb(a->io + ATA_DRIVE, 0xE0 | (a->slave << 4));
    delay400(a->ctrl);
    outb(a->io + ATA_CMD, a->lba48 ? 0xEA : 0xE7);
    wait_not_busy(a->io, 50000000);
    mutex_unlock(a->lock);
    return 0;
}

static void probe(uint16_t io, uint16_t ctrl, int slave, int chan, int *count) {
    outb(ctrl, 0x02);                               /* nIEN: no interrupts */
    outb(io + ATA_DRIVE, 0xA0 | (slave << 4));
    delay400(ctrl);
    uint8_t st = inb(io + ATA_STATUS);
    if (st == 0xFF || st == 0x00) return;           /* floating bus / no device */
    outb(io + ATA_COUNT, 0);
    outb(io + ATA_LBA0, 0);
    outb(io + ATA_LBA1, 0);
    outb(io + ATA_LBA2, 0);
    outb(io + ATA_CMD, 0xEC);                       /* IDENTIFY */
    delay400(ctrl);
    st = inb(io + ATA_STATUS);
    if (st == 0) return;
    if (wait_not_busy(io, 1000000) < 0) return;
    if (inb(io + ATA_LBA1) || inb(io + ATA_LBA2)) return;   /* ATAPI / SATA signature: not a plain disk */
    if (wait_drq(io) < 0) return;
    uint16_t id[256];
    insw(io + ATA_DATA, id, 256);
    ata_disk_t *a = kzalloc(sizeof(ata_disk_t));
    a->io = io;
    a->ctrl = ctrl;
    a->slave = slave;
    a->lock = &channel_lock[chan];
    a->lba48 = (id[83] & (1 << 10)) != 0;
    uint64_t sectors = a->lba48 ? ((uint64_t)id[103] << 48 | (uint64_t)id[102] << 32 | (uint64_t)id[101] << 16 | id[100])
                                : ((uint32_t)id[61] << 16 | id[60]);
    if (!sectors) { kfree(a); return; }
    /* model string: byte-swapped words */
    for (int i = 0; i < 20; i++) {
        a->dev.model[i * 2] = (char)(id[27 + i] >> 8);
        a->dev.model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    a->dev.model[40] = 0;
    for (int i = 39; i >= 0 && a->dev.model[i] == ' '; i--) a->dev.model[i] = 0;
    /* bus master DMA if the controller supports it and the drive does multiword/UDMA */
    if (bm_base[chan] && (id[49] & (1 << 8)) && !cmdline_has("nodma")) {
        uint64_t prd = pmm_alloc_contig(1, 0x100000000ULL);
        /* a PRD region must not cross a 64 KiB boundary: take the aligned 64 KiB inside 128 KiB */
        uint64_t area = pmm_alloc_contig(32, 0x100000000ULL);
        if (prd && area) {
            uint64_t buf = ALIGN_UP(area, 0x10000);
            a->bm = bm_base[chan];
            a->prd = P2V(prd);
            a->prd_phys = prd;
            a->dma_buf = P2V(buf);
            a->dma_phys = buf;
        }
    }
    outb(ctrl, a->bm ? 0x00 : 0x02);   /* DMA completion is signalled through INTRQ (masked at the PIC) */
    snprintf(a->dev.name, sizeof(a->dev.name), "hd%c", 'a' + (*count)++);
    a->dev.nsectors = sectors;
    a->dev.read = ata_read;
    a->dev.write = ata_write;
    a->dev.flush = ata_flush;
    a->dev.priv = a;
    klog("[ata] %s: %s, %lu MiB%s%s\n", a->dev.name, a->dev.model, sectors / 2048, a->lba48 ? " (LBA48)" : "",
         a->bm ? ", DMA" : ", PIO");
    blk_register(&a->dev);
    blk_scan_partitions(&a->dev);
}

void ata_init(void) {
    uint16_t io[2] = { 0x1F0, 0x170 }, ctrl[2] = { 0x3F6, 0x376 };
    pci_dev_t *ide = pci_find_class(0x01, 0x01);
    if (ide) {
        /* native mode channels use BARs */
        if ((ide->prog_if & 0x01) && (ide->bar[0] & ~3u)) { io[0] = ide->bar[0] & ~3u; ctrl[0] = (ide->bar[1] & ~3u) + 2; }
        if ((ide->prog_if & 0x04) && (ide->bar[2] & ~3u)) { io[1] = ide->bar[2] & ~3u; ctrl[1] = (ide->bar[3] & ~3u) + 2; }
        /* bus master IDE (prog_if bit 7) */
        if ((ide->prog_if & 0x80) && (ide->bar[4] & 1) && (ide->bar[4] & ~3u)) {
            pci_enable_bus_master(ide);
            bm_base[0] = ide->bar[4] & ~3u;
            bm_base[1] = bm_base[0] + 8;
        }
    }
    int count = 0;
    for (int ch = 0; ch < 2; ch++)
        for (int s = 0; s < 2; s++) probe(io[ch], ctrl[ch], s, ch, &count);
    if (!count) klog("[ata] no ATA disks found\n");
}
