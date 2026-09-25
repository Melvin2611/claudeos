/* Block device layer: registry, write-through sector cache, MBR partitions */
#include <kernel.h>
#include <blk.h>
#include <sched.h>

static blkdev_t *devices;

#define CACHE_SLOTS 2048          /* 1 MiB of cached sectors */
#define HASH_SIZE 1024

typedef struct cache_ent {
    blkdev_t *dev;                /* whole-disk device */
    uint64_t lba;
    uint64_t last_use;
    bool valid;
    bool dirty;                   /* modified in memory, not yet on disk */
    int hnext;
    uint8_t data[512];
} cache_ent_t;

static cache_ent_t *cache;
static int hash_head[HASH_SIZE];
static uint64_t use_counter;
static mutex_t cache_lock;

void blk_register(blkdev_t *d) {
    blkdev_t **pp = &devices;
    while (*pp) pp = &(*pp)->next;
    d->next = 0;
    *pp = d;
}

blkdev_t *blk_list(void) { return devices; }

void blk_unregister(blkdev_t *d) {
    for (blkdev_t **pp = &devices; *pp; pp = &(*pp)->next) {
        if (*pp == d) { *pp = d->next; return; }
    }
}

blkdev_t *blk_find(const char *name) {
    for (blkdev_t *d = devices; d; d = d->next)
        if (!strcmp(d->name, name)) return d;
    return 0;
}

static void cache_init(void) {
    if (cache) return;
    cache = vmalloc(sizeof(cache_ent_t) * CACHE_SLOTS);
    for (int i = 0; i < HASH_SIZE; i++) hash_head[i] = -1;
    for (int i = 0; i < CACHE_SLOTS; i++) cache[i].hnext = -1;
}

static int hash(blkdev_t *d, uint64_t lba) { return (int)(((uint64_t)d * 31 + lba * 2654435761u) % HASH_SIZE); }

static cache_ent_t *lookup(blkdev_t *d, uint64_t lba) {
    for (int i = hash_head[hash(d, lba)]; i >= 0; i = cache[i].hnext)
        if (cache[i].valid && cache[i].dev == d && cache[i].lba == lba) return &cache[i];
    return 0;
}

static void unhash(int idx) {
    cache_ent_t *e = &cache[idx];
    if (!e->valid) return;
    int *pp = &hash_head[hash(e->dev, e->lba)];
    while (*pp >= 0 && *pp != idx) pp = &cache[*pp].hnext;
    if (*pp == idx) *pp = e->hnext;
    e->valid = false;
}

static cache_ent_t *insert(blkdev_t *d, uint64_t lba, const void *data) {
    /* pick the least recently used slot (sampling to stay cheap) */
    int best = -1;
    uint64_t best_use = ~0ULL;
    int start = (int)(use_counter * 7 % CACHE_SLOTS);
    for (int k = 0; k < 64; k++) {
        int i = (start + k * 37) % CACHE_SLOTS;
        if (!cache[i].valid) { best = i; break; }
        /* prefer clean entries: evicting a dirty one costs a disk write */
        uint64_t use = cache[i].last_use + (cache[i].dirty ? 1000000 : 0);
        if (use < best_use) { best_use = use; best = i; }
    }
    if (cache[best].valid && cache[best].dirty) {
        cache[best].dev->write(cache[best].dev, cache[best].lba, 1, cache[best].data);
        cache[best].dirty = false;
    }
    unhash(best);
    cache_ent_t *e = &cache[best];
    e->dirty = false;
    e->dev = d;
    e->lba = lba;
    e->valid = true;
    e->last_use = ++use_counter;
    memcpy(e->data, data, 512);
    int h = hash(d, lba);
    e->hnext = hash_head[h];
    hash_head[h] = best;
    return e;
}

static blkdev_t *root_of(blkdev_t *d, uint64_t *lba) {
    while (d->parent) { *lba += d->offset; d = d->parent; }
    return d;
}

int blk_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf) {
    if (lba + count > d->nsectors) return -EIO;
    blkdev_t *disk = root_of(d, &lba);
    uint8_t *p = buf;
    mutex_lock(&cache_lock);
    cache_init();
    uint32_t i = 0;
    while (i < count) {
        cache_ent_t *e = lookup(disk, lba + i);
        if (e) {
            memcpy(p + i * 512, e->data, 512);
            e->last_use = ++use_counter;
            i++;
            continue;
        }
        /* read a run of uncached sectors at once */
        uint32_t run = 1;
        while (i + run < count && run < 128 && !lookup(disk, lba + i + run)) run++;
        int r = disk->read(disk, lba + i, run, p + i * 512);
        if (r < 0) { mutex_unlock(&cache_lock); return r; }
        for (uint32_t k = 0; k < run; k++) insert(disk, lba + i + k, p + (i + k) * 512);
        i += run;
    }
    mutex_unlock(&cache_lock);
    return 0;
}

int blk_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf) {
    if (lba + count > d->nsectors) return -EIO;
    blkdev_t *disk = root_of(d, &lba);
    mutex_lock(&cache_lock);
    cache_init();
    int r = disk->write(disk, lba, count, buf);
    if (r == 0) {
        const uint8_t *p = buf;
        for (uint32_t i = 0; i < count; i++) {
            cache_ent_t *e = lookup(disk, lba + i);
            if (e) { memcpy(e->data, p + i * 512, 512); e->last_use = ++use_counter; e->dirty = false; }
            else if (count <= 8) insert(disk, lba + i, p + i * 512);
        }
    }
    mutex_unlock(&cache_lock);
    return r;
}

/* write one sector into the cache only (metadata); it reaches the disk on flush/eviction */
int blk_write_lazy(blkdev_t *d, uint64_t lba, const void *buf) {
    if (lba >= d->nsectors) return -EIO;
    blkdev_t *disk = root_of(d, &lba);
    mutex_lock(&cache_lock);
    cache_init();
    cache_ent_t *e = lookup(disk, lba);
    if (!e) e = insert(disk, lba, buf);
    else memcpy(e->data, buf, 512);
    e->dirty = true;
    e->last_use = ++use_counter;
    mutex_unlock(&cache_lock);
    return 0;
}

static int write_back(blkdev_t *disk) {
    /* collect dirty sectors of this disk, sorted, and write them in runs */
    int n = 0;
    int *idx = kmalloc(sizeof(int) * CACHE_SLOTS);
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (cache[i].valid && cache[i].dirty && cache[i].dev == disk) idx[n++] = i;
    for (int i = 1; i < n; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && cache[idx[j]].lba > cache[v].lba) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    int r = 0;
    uint8_t *buf = kmalloc(64 * 512);
    for (int i = 0; i < n;) {
        int run = 1;
        while (i + run < n && run < 64 && cache[idx[i + run]].lba == cache[idx[i]].lba + run) run++;
        for (int k = 0; k < run; k++) memcpy(buf + k * 512, cache[idx[i + k]].data, 512);
        if (disk->write(disk, cache[idx[i]].lba, run, buf) < 0) r = -EIO;
        for (int k = 0; k < run; k++) cache[idx[i + k]].dirty = false;
        i += run;
    }
    kfree(buf);
    kfree(idx);
    return r;
}

int blk_flush(blkdev_t *d) {
    uint64_t dummy = 0;
    blkdev_t *disk = root_of(d, &dummy);
    mutex_lock(&cache_lock);
    int r = cache ? write_back(disk) : 0;
    mutex_unlock(&cache_lock);
    if (disk->flush) disk->flush(disk);
    return r;
}

/* ---- partitions ---- */
static int part_read(blkdev_t *d, uint64_t lba, uint32_t n, void *buf) { return d->parent->read(d->parent, lba + d->offset, n, buf); }
static int part_write(blkdev_t *d, uint64_t lba, uint32_t n, const void *buf) { return d->parent->write(d->parent, lba + d->offset, n, buf); }

static void add_partition(blkdev_t *disk, int num, uint64_t start, uint64_t size, const char *type) {
    if (!size || start + size > disk->nsectors) return;
    blkdev_t *p = kzalloc(sizeof(blkdev_t));
    size_t l = strlen(disk->name);
    bool digit = l && disk->name[l - 1] >= '0' && disk->name[l - 1] <= '9';
    snprintf(p->name, sizeof(p->name), "%s%s%d", disk->name, digit ? "p" : "", num);
    strlcpy(p->model, disk->model, sizeof(p->model));
    p->parent = disk;
    p->offset = start;
    p->nsectors = size;
    p->read = part_read;
    p->write = part_write;
    p->flush = 0;
    klog("[blk] %s: %s partition, %lu MiB\n", p->name, type, size / 2048);
    blk_register(p);
}

/* GUID partition table (behind a protective MBR) */
static bool scan_gpt(blkdev_t *disk) {
    uint8_t *hdr = kmalloc(512);
    if (blk_read(disk, 1, 1, hdr) < 0 || memcmp(hdr, "EFI PART", 8)) { kfree(hdr); return false; }
    uint64_t entries_lba = *(uint64_t *)(hdr + 72);
    uint32_t count = *(uint32_t *)(hdr + 80), esize = *(uint32_t *)(hdr + 84);
    kfree(hdr);
    if (esize < 128 || esize > 512 || count > 256) return false;
    uint32_t bytes = count * esize;
    uint32_t secs = (bytes + 511) / 512;
    uint8_t *tab = kmalloc(secs * 512);
    if (blk_read(disk, entries_lba, secs, tab) < 0) { kfree(tab); return false; }
    static const uint8_t zero[16];
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *e = tab + i * esize;
        if (!memcmp(e, zero, 16)) continue;
        uint64_t first = *(uint64_t *)(e + 32), last = *(uint64_t *)(e + 40);
        /* a few well-known type GUIDs (first 4 bytes, little endian) */
        uint32_t g = *(uint32_t *)e;
        const char *type = g == 0xEBD0A0A2 ? "Basic data" : g == 0x0FC63DAF ? "Linux" : g == 0xC12A7328 ? "EFI system" :
                           g == 0x0657FD6D ? "Linux swap" : g == 0xE3C9E316 ? "MS reserved" : "GPT";
        add_partition(disk, (int)i + 1, first, last - first + 1, type);
    }
    kfree(tab);
    return true;
}

void blk_scan_partitions(blkdev_t *disk) {
    uint8_t *mbr = kmalloc(512);
    if (blk_read(disk, 0, 1, mbr) < 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) { kfree(mbr); return; }
    /* a FAT boot sector (superfloppy) also carries 0x55AA: detect it by its BPB jump + "FAT" signature */
    if ((mbr[0] == 0xEB || mbr[0] == 0xE9) && (!memcmp(mbr + 82, "FAT", 3) || !memcmp(mbr + 54, "FAT", 3))) {
        kfree(mbr);
        return;
    }
    if (mbr[446 + 4] == 0xEE && scan_gpt(disk)) { kfree(mbr); return; }
    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t start = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t size = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
        if (!type) continue;
        char tname[16];
        snprintf(tname, sizeof(tname), "type %02x", type);
        add_partition(disk, i + 1, start, size, tname);
    }
    kfree(mbr);
}
