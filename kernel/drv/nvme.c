/* NVMe driver: admin queue + one I/O queue pair, one command in flight, bounce buffer,
 * MSI/MSI-X completion interrupts (polling fallback). */
#include <kernel.h>
#include <blk.h>
#include <pci.h>
#include <sched.h>
#include <mm.h>
#include "drivers.h"

#define REG_CAP  0x00
#define REG_VS   0x08
#define REG_INTMS 0x0C
#define REG_CC   0x14
#define REG_CSTS 0x1C
#define REG_AQA  0x24
#define REG_ASQ  0x28
#define REG_ACQ  0x30

#define QDEPTH 64
#define BOUNCE_BYTES (128 * 1024)

typedef struct {
    uint32_t cdw0, nsid, rsvd[2];
    uint64_t mptr, prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_cmd_t;

typedef struct {
    uint32_t result, rsvd;
    uint16_t sq_head, sq_id, cid, status;
} nvme_cpl_t;

typedef struct {
    nvme_cmd_t *sq;
    nvme_cpl_t *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t sq_tail, cq_head, phase, id;
} nvme_queue_t;

typedef struct {
    volatile uint8_t *regs;
    uint32_t dstrd;
    nvme_queue_t admin, io;
    uint16_t cid;
    bool irq;
    waitq_t wq;
    mutex_t lock;
    uint8_t *bounce;
    uint64_t bounce_phys;
    uint64_t *prp_list;                 /* PRP list describing the bounce buffer */
    uint64_t prp_list_phys;
    uint32_t lba_size;
    uint64_t nlba;
    uint32_t nsid;
    blkdev_t dev;
    int index;
} nvme_t;

static int nvme_count;

static inline uint32_t r32(nvme_t *n, int reg) { return *(volatile uint32_t *)(n->regs + reg); }
static inline void w32(nvme_t *n, int reg, uint32_t v) { *(volatile uint32_t *)(n->regs + reg) = v; }
static inline uint64_t r64(nvme_t *n, int reg) { return r32(n, reg) | ((uint64_t)r32(n, reg + 4) << 32); }
static inline void w64(nvme_t *n, int reg, uint64_t v) { w32(n, reg, (uint32_t)v); w32(n, reg + 4, v >> 32); }

static void doorbell(nvme_t *n, int qid, bool cq, uint32_t v) {
    w32(n, 0x1000 + (2 * qid + (cq ? 1 : 0)) * (4 << n->dstrd), v);
}

static void nvme_irq(void *ctx) {
    nvme_t *n = ctx;
    wq_wake_all(&n->wq);
}

static bool queue_alloc(nvme_queue_t *q, uint16_t id) {
    q->sq_phys = pmm_alloc_contig(1, 0x100000000ULL);
    q->cq_phys = pmm_alloc_contig(1, 0x100000000ULL);
    if (!q->sq_phys || !q->cq_phys) return false;
    q->sq = P2V(q->sq_phys);
    q->cq = P2V(q->cq_phys);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    q->id = id;
    return true;
}

/* submit one command and wait for its completion; returns the NVMe status (0 = success) */
static int submit(nvme_t *n, nvme_queue_t *q, nvme_cmd_t *c, uint32_t *result) {
    c->cdw0 = (c->cdw0 & 0xFFFF) | ((uint32_t)++n->cid << 16);
    q->sq[q->sq_tail] = *c;
    q->sq_tail = (q->sq_tail + 1) % QDEPTH;
    doorbell(n, q->id, false, q->sq_tail);
    uint64_t end = uptime_ms() + 10000;
    for (;;) {
        volatile nvme_cpl_t *e = &q->cq[q->cq_head];
        if ((e->status & 1) == q->phase) {
            uint16_t st = e->status >> 1;
            if (result) *result = e->result;
            q->cq_head = (q->cq_head + 1) % QDEPTH;
            if (q->cq_head == 0) q->phase ^= 1;
            doorbell(n, q->id, true, q->cq_head);
            return st;
        }
        if (uptime_ms() > end) {
            klog("[nvme] command %02x timed out\n", c->cdw0 & 0xFF);
            return -1;
        }
        uint64_t f = irq_save();
        if ((e->status & 1) != q->phase) {
            if (n->irq && q->id) wq_wait_timeout(&n->wq, 20);
            else { irq_restore(f); if (q->id) yield(); else cpu_pause(); f = irq_save(); }
        }
        irq_restore(f);
    }
}

static int admin(nvme_t *n, uint8_t opc, uint32_t nsid, uint64_t prp1, uint32_t cdw10, uint32_t cdw11, uint32_t *res) {
    nvme_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.cdw0 = opc;
    c.nsid = nsid;
    c.prp1 = prp1;
    c.cdw10 = cdw10;
    c.cdw11 = cdw11;
    return submit(n, &n->admin, &c, res);
}

/* read/write whole device blocks through the bounce buffer */
static int io_blocks(nvme_t *n, bool write, uint64_t blk, uint32_t count) {
    nvme_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.cdw0 = write ? 0x01 : 0x02;
    c.nsid = n->nsid;
    uint32_t bytes = count * n->lba_size;
    c.prp1 = n->bounce_phys;
    if (bytes > PAGE_SIZE * 2) c.prp2 = n->prp_list_phys;
    else if (bytes > PAGE_SIZE) c.prp2 = n->bounce_phys + PAGE_SIZE;
    c.cdw10 = (uint32_t)blk;
    c.cdw11 = (uint32_t)(blk >> 32);
    c.cdw12 = count - 1;
    int st = submit(n, &n->io, &c, 0);
    if (st) {
        klog("[nvme] %s error, status %x at block %lu\n", write ? "write" : "read", st, blk);
        return -EIO;
    }
    return 0;
}

/* the block layer uses 512-byte sectors; larger device blocks need read-modify-write */
static int nvme_rw(blkdev_t *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    nvme_t *n = d->priv;
    uint32_t spb = n->lba_size / 512;   /* sectors per device block */
    uint8_t *b = buf;
    mutex_lock(&n->lock);
    while (count) {
        uint64_t blk = lba / spb;
        uint32_t off = (uint32_t)(lba % spb);
        uint32_t max_sec = BOUNCE_BYTES / 512 - off;
        uint32_t nsec = MIN(count, max_sec);
        uint32_t nblk = (off + nsec + spb - 1) / spb;
        int r = 0;
        if (write) {
            bool partial = off || (off + nsec) % spb;
            if (partial) r = io_blocks(n, false, blk, nblk);
            if (!r) {
                memcpy(n->bounce + off * 512, b, nsec * 512);
                r = io_blocks(n, true, blk, nblk);
            }
        } else {
            r = io_blocks(n, false, blk, nblk);
            if (!r) memcpy(b, n->bounce + off * 512, nsec * 512);
        }
        if (r) { mutex_unlock(&n->lock); return r; }
        b += nsec * 512;
        lba += nsec;
        count -= nsec;
    }
    mutex_unlock(&n->lock);
    return 0;
}

static int nvme_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf) { return nvme_rw(d, lba, count, buf, false); }
static int nvme_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf) {
    return nvme_rw(d, lba, count, (void *)buf, true);
}
static int nvme_flush(blkdev_t *d) {
    nvme_t *n = d->priv;
    nvme_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.cdw0 = 0x00;
    c.nsid = n->nsid;
    mutex_lock(&n->lock);
    int st = submit(n, &n->io, &c, 0);
    mutex_unlock(&n->lock);
    return st ? -EIO : 0;
}

static void nvme_init_ctrl(pci_dev_t *pd) {
    nvme_t *n = kzalloc(sizeof(nvme_t));
    pci_enable_bus_master(pd);
    n->regs = pci_map_bar(pd, 0, 0);
    if (!n->regs) { klog("[nvme] cannot map BAR0\n"); return; }
    uint64_t cap = r64(n, REG_CAP);
    n->dstrd = (cap >> 32) & 0xF;
    uint32_t timeout = ((cap >> 24) & 0xFF) * 500 + 500;
    if (((cap >> 37) & 1) == 0) { klog("[nvme] controller lacks the NVM command set\n"); return; }

    /* reset */
    w32(n, REG_CC, r32(n, REG_CC) & ~1u);
    for (uint32_t i = 0; i < timeout && (r32(n, REG_CSTS) & 1); i++) pit_delay_ms(1);
    if (!queue_alloc(&n->admin, 0) || !queue_alloc(&n->io, 1)) return;
    w32(n, REG_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
    w64(n, REG_ASQ, n->admin.sq_phys);
    w64(n, REG_ACQ, n->admin.cq_phys);
    w32(n, REG_CC, (4u << 20) | (6u << 16) | 1);  /* IOCQES 16 B, IOSQES 64 B, 4 KiB pages, enable */
    uint32_t i;
    for (i = 0; i < timeout && !(r32(n, REG_CSTS) & 1); i++) pit_delay_ms(1);
    if (!(r32(n, REG_CSTS) & 1) || (r32(n, REG_CSTS) & 2)) { klog("[nvme] controller did not become ready\n"); return; }

    /* DMA buffers */
    n->bounce_phys = pmm_alloc_contig(BOUNCE_BYTES / PAGE_SIZE, 0x100000000ULL);
    n->prp_list_phys = pmm_alloc_contig(1, 0x100000000ULL);
    if (!n->bounce_phys || !n->prp_list_phys) return;
    n->bounce = P2V(n->bounce_phys);
    n->prp_list = P2V(n->prp_list_phys);
    for (uint32_t p = 1; p < BOUNCE_BYTES / PAGE_SIZE; p++) n->prp_list[p - 1] = n->bounce_phys + p * PAGE_SIZE;

    /* identify controller */
    if (admin(n, 0x06, 0, n->bounce_phys, 1, 0, 0)) { klog("[nvme] identify failed\n"); return; }
    char model[41];
    memcpy(model, n->bounce + 24, 40);
    model[40] = 0;
    for (int k = 39; k >= 0 && (model[k] == ' ' || !model[k]); k--) model[k] = 0;
    uint8_t mdts = n->bounce[77];
    if (mdts && ((uint64_t)PAGE_SIZE << mdts) < BOUNCE_BYTES) klog("[nvme] warning: small MDTS %u\n", mdts);

    /* first active namespace */
    n->nsid = 1;
    if (admin(n, 0x06, 0, n->bounce_phys, 2, 0, 0) == 0 && *(uint32_t *)n->bounce) n->nsid = *(uint32_t *)n->bounce;
    if (admin(n, 0x06, n->nsid, n->bounce_phys, 0, 0, 0)) { klog("[nvme] identify namespace failed\n"); return; }
    n->nlba = *(uint64_t *)n->bounce;
    uint8_t flbas = n->bounce[26] & 0xF;
    uint32_t lbaf = *(uint32_t *)(n->bounce + 128 + flbas * 4);
    n->lba_size = 1u << ((lbaf >> 16) & 0xFF);
    if (n->lba_size < 512 || n->lba_size > 4096) { klog("[nvme] unsupported block size %u\n", n->lba_size); return; }

    /* interrupts, then the I/O queue pair (completion queue first) */
    n->irq = pci_enable_msi(pd, nvme_irq, n);
    if (admin(n, 0x05, 0, n->io.cq_phys, ((QDEPTH - 1) << 16) | 1, n->irq ? 3 : 1, 0) ||
        admin(n, 0x01, 0, n->io.sq_phys, ((QDEPTH - 1) << 16) | 1, (1 << 16) | 1, 0)) {
        klog("[nvme] cannot create I/O queues\n");
        return;
    }
    blkdev_t *d = &n->dev;
    n->index = nvme_count++;
    snprintf(d->name, sizeof(d->name), "nvme%dn1", n->index);
    strlcpy(d->model, model, sizeof(d->model));
    d->nsectors = n->nlba * (n->lba_size / 512);
    d->read = nvme_read;
    d->write = nvme_write;
    d->flush = nvme_flush;
    d->priv = n;
    uint32_t vs = r32(n, REG_VS);
    klog("[nvme] %s: %s, %lu MiB, %u-byte blocks, NVMe %u.%u, %s\n", d->name, model, d->nsectors / 2048,
         n->lba_size, vs >> 16, (vs >> 8) & 0xFF, n->irq ? "MSI" : "polling");
    blk_register(d);
    blk_scan_partitions(d);
}

void nvme_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *d = pci_get(i);
        if (d->class_code == 0x01 && d->subclass == 0x08 && d->prog_if == 0x02) nvme_init_ctrl(d);
    }
}
