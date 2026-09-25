/* USB mass storage (bulk-only transport, SCSI transparent command set): USB sticks and card
 * readers become block devices "usbN"; their file systems are mounted under /mnt. */
#include <kernel.h>
#include <usb.h>
#include <blk.h>
#include <sched.h>
#include <mm.h>
#include <vfs.h>

#define MAX_XFER (64 * 1024)

typedef struct {
    usb_device_t *dev;
    usb_endpoint_t *in, *out;
    uint8_t iface;
    uint32_t tag;
    uint32_t block_size;
    blkdev_t blk;
    mutex_t lock;
    bool gone;
} msc_t;

static int msc_count;

typedef struct PACKED {
    uint32_t sig, tag, len;
    uint8_t flags, lun, cblen;
    uint8_t cb[16];
} cbw_t;

typedef struct PACKED {
    uint32_t sig, tag, residue;
    uint8_t status;
} csw_t;

static void reset_recovery(msc_t *m) {
    usb_control(m->dev, 0x21, 0xFF, 0, m->iface, 0, 0);   /* bulk-only mass storage reset */
    usb_clear_halt(m->in);
    usb_clear_halt(m->out);
}

/* one SCSI command; returns 0, or -EIO (check condition / transport error) */
static int scsi(msc_t *m, const uint8_t *cb, int cblen, void *data, uint32_t len, bool in) {
    if (m->gone) return -ENODEV;
    cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.sig = 0x43425355;
    cbw.tag = ++m->tag;
    cbw.len = len;
    cbw.flags = in ? 0x80 : 0;
    cbw.cblen = (uint8_t)cblen;
    memcpy(cbw.cb, cb, cblen);
    if (usb_bulk(m->out, &cbw, sizeof(cbw), 0) < 0) { reset_recovery(m); return -EIO; }
    if (len) {
        uint32_t got = 0;
        int r = usb_bulk(in ? m->in : m->out, data, len, &got);
        if (r == -EPIPE) usb_clear_halt(in ? m->in : m->out);
        else if (r < 0) { reset_recovery(m); return -EIO; }
    }
    csw_t csw;
    uint32_t got = 0;
    int r = usb_bulk(m->in, &csw, sizeof(csw), &got);
    if (r == -EPIPE) {
        usb_clear_halt(m->in);
        r = usb_bulk(m->in, &csw, sizeof(csw), &got);
    }
    if (r < 0 || got < sizeof(csw) || csw.sig != 0x53425355) { reset_recovery(m); return -EIO; }
    return csw.status == 0 ? 0 : -EIO;
}

static int msc_rw(blkdev_t *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    msc_t *m = d->priv;
    uint8_t *b = buf;
    mutex_lock(&m->lock);
    while (count) {
        uint32_t n = MIN(count, (uint32_t)(MAX_XFER / 512));
        uint8_t cb[10] = { write ? 0x2A : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                           (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        int r = scsi(m, cb, 10, b, n * 512, !write);
        if (r < 0) r = scsi(m, cb, 10, b, n * 512, !write);   /* one retry */
        if (r < 0) {
            mutex_unlock(&m->lock);
            return r;
        }
        b += n * 512;
        lba += n;
        count -= n;
    }
    mutex_unlock(&m->lock);
    return 0;
}

static int msc_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf) { return msc_rw(d, lba, count, buf, false); }
static int msc_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf) {
    return msc_rw(d, lba, count, (void *)buf, true);
}
static int msc_flush(blkdev_t *d) {
    msc_t *m = d->priv;
    uint8_t cb[10] = { 0x35 };                               /* SYNCHRONIZE CACHE(10) */
    mutex_lock(&m->lock);
    scsi(m, cb, 10, 0, 0, false);
    mutex_unlock(&m->lock);
    return 0;
}

void wm_notify(const char *title, const char *text, const char *icon);

static void msc_start(usb_device_t *d, void *ctx) {
    msc_t *m = ctx;
    uint8_t buf[64];
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    char vendor[9] = "", product[17] = "";
    if (scsi(m, inq, 6, buf, 36, true) == 0) {
        memcpy(vendor, buf + 8, 8);
        memcpy(product, buf + 16, 16);
        for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
        for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;
    }
    /* wait for the medium (card readers, slow sticks) */
    uint8_t tur[6] = { 0 };
    for (int i = 0; i < 20 && scsi(m, tur, 6, 0, 0, false) < 0; i++) {
        uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
        scsi(m, rs, 6, buf, 18, true);                       /* REQUEST SENSE clears the condition */
        sleep_ms(100);
    }
    uint8_t rc[10] = { 0x25 };                               /* READ CAPACITY(10) */
    if (scsi(m, rc, 10, buf, 8, true) < 0) { klog("[usb]   storage: no medium\n"); return; }
    uint64_t last = ((uint32_t)buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
    m->block_size = ((uint32_t)buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
    if (m->block_size != 512) { klog("[usb]   storage: %u-byte blocks are not supported\n", m->block_size); return; }
    blkdev_t *b = &m->blk;
    snprintf(b->name, sizeof(b->name), "usb%d", msc_count++);
    snprintf(b->model, sizeof(b->model), "%s %s", vendor, product);
    b->nsectors = last + 1;
    b->read = msc_read;
    b->write = msc_write;
    b->flush = msc_flush;
    b->priv = m;
    klog("[usb]   %s: %s, %lu MiB\n", b->name, b->model, b->nsectors / 2048);
    blk_register(b);
    blk_scan_partitions(b);
    /* mount every file system found on the stick */
    int mounted = 0;
    char where[48] = "";
    for (blkdev_t *p = blk_list(); p; p = p->next) {
        if ((p != b && p->parent != b) || p->mounted) continue;
        bool parts = false;
        for (blkdev_t *q = blk_list(); q; q = q->next) if (q->parent == p) parts = true;
        if (parts) continue;
        char path[48];
        snprintf(path, sizeof(path), "/mnt/%s", p->name);
        vfs_mkdir("/mnt");
        vfs_mkdir(path);
        const char *type;
        if (fs_mount_any(p, path, &type) == 0) {
            klog("[usb]   %s (%s) mounted on %s\n", p->name, type, path);
            if (!mounted++) strlcpy(where, path, sizeof(where));
        } else {
            vfs_rmdir(path);
        }
    }
    char msg[128];
    if (mounted) snprintf(msg, sizeof(msg), "%s is available at %s", b->model[0] ? b->model : "USB drive", where);
    else snprintf(msg, sizeof(msg), "%s connected (no supported file system)", b->model[0] ? b->model : "USB drive");
    wm_notify("USB drive", msg, "folder");
}

static void msc_stop(usb_device_t *d, void *ctx) {
    UNUSED(d);
    msc_t *m = ctx;
    m->gone = true;
    if (m->blk.name[0]) {
        storage_disk_removed(&m->blk);
        char msg[96];
        snprintf(msg, sizeof(msg), "%s was removed", m->blk.name);
        wm_notify("USB drive", msg, "folder");
    }
}

bool usb_msc_probe(usb_device_t *d, usb_interface_desc_t *intf, uint8_t *extra, int extra_len) {
    if (intf->bInterfaceClass != 8 || intf->bInterfaceProtocol != 0x50) return false;
    if (intf->bInterfaceSubClass != 6 && intf->bInterfaceSubClass != 5 && intf->bInterfaceSubClass != 2) return false;
    usb_endpoint_desc_t *in = 0, *out = 0;
    for (uint8_t *p = extra; p + 2 <= extra + extra_len && p[0]; p += p[0]) {
        if (p[1] != USB_DT_ENDPOINT || (p[3] & 3) != 2) continue;
        if (p[2] & 0x80) in = (usb_endpoint_desc_t *)p;
        else out = (usb_endpoint_desc_t *)p;
    }
    if (!in || !out) return false;
    msc_t *m = kzalloc(sizeof(msc_t));
    m->dev = d;
    m->iface = intf->bInterfaceNumber;
    m->in = usb_add_endpoint(d, in);
    m->out = usb_add_endpoint(d, out);
    if (!m->in || !m->out) { kfree(m); return false; }
    usb_bind(d, msc_start, msc_stop, m);
    return true;
}
