/* FAT32 file system with long file names (read/write) and a simple formatter */
#include <kernel.h>
#include <vfs.h>
#include <blk.h>
#include <claudeos/font.h>

#define EOC 0x0FFFFFF8u
#define ATTR_RO 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR 0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN 0x0F

typedef struct __attribute__((packed)) {
    char name[11];
    uint8_t attr, ntres, ctime_tenth;
    uint16_t ctime, cdate, adate, clus_hi, mtime, mdate, clus_lo;
    uint32_t size;
} fat_dirent_t;

typedef struct __attribute__((packed)) {
    uint8_t ord;
    uint16_t name1[5];
    uint8_t attr, type, chksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} fat_lfn_t;

typedef struct fat_fs fat_fs_t;

typedef struct fat_node {
    fat_fs_t *fs;
    vnode_t *vn;
    uint32_t first;           /* first cluster (0 = none) */
    uint32_t dir_first;       /* first cluster of the parent directory */
    uint32_t dirent_off;      /* byte offset of the short entry in the parent */
    bool is_root;
    uint32_t last_idx, last_clus;   /* chain walk cache */
    uint64_t rd_index, rd_off;      /* readdir resume point */
    struct fat_node *next;
} fat_node_t;

struct fat_fs {
    blkdev_t *dev;
    uint32_t spc, reserved, nfats, fat_size, root_clus, total_sectors, data_start, nclusters, fsinfo;
    uint32_t cbytes;
    uint32_t free_count, next_free;
    fat_node_t *nodes;
    char label[12];
};

static const vnode_ops_t fat_ops;

/* ------------------------------------------------------------------ FAT table */
static uint32_t clus_lba(fat_fs_t *fs, uint32_t c) { return fs->data_start + (c - 2) * fs->spc; }

static uint32_t fat_get(fat_fs_t *fs, uint32_t c) {
    uint8_t sec[512];
    uint32_t off = c * 4;
    if (blk_read(fs->dev, fs->reserved + off / 512, 1, sec) < 0) return EOC;
    return *(uint32_t *)(sec + off % 512) & 0x0FFFFFFF;
}

static int fat_set(fat_fs_t *fs, uint32_t c, uint32_t v) {
    uint8_t sec[512];
    uint32_t off = c * 4;
    for (uint32_t f = 0; f < fs->nfats; f++) {
        uint32_t lba = fs->reserved + f * fs->fat_size + off / 512;
        if (blk_read(fs->dev, lba, 1, sec) < 0) return -EIO;
        uint32_t *p = (uint32_t *)(sec + off % 512);
        *p = (*p & 0xF0000000u) | (v & 0x0FFFFFFF);
        if (blk_write_lazy(fs->dev, lba, sec) < 0) return -EIO;
    }
    return 0;
}

static int zero_cluster(fat_fs_t *fs, uint32_t c) {
    uint8_t *z = kzalloc(fs->cbytes);
    int r = blk_write(fs->dev, clus_lba(fs, c), fs->spc, z);
    kfree(z);
    return r;
}

static uint32_t alloc_cluster(fat_fs_t *fs, uint32_t prev, bool zero) {
    uint32_t start = fs->next_free >= 2 && fs->next_free < fs->nclusters + 2 ? fs->next_free : 2;
    uint32_t c = start;
    for (uint32_t n = 0; n < fs->nclusters; n++) {
        if (fat_get(fs, c) == 0) {
            if (fat_set(fs, c, 0x0FFFFFFF) < 0) return 0;
            if (prev) fat_set(fs, prev, c);
            if (zero) zero_cluster(fs, c);
            fs->next_free = c + 1;
            if (fs->free_count != 0xFFFFFFFF && fs->free_count) fs->free_count--;
            return c;
        }
        if (++c >= fs->nclusters + 2) c = 2;
    }
    return 0;
}

static void free_chain(fat_fs_t *fs, uint32_t c) {
    while (c >= 2 && c < EOC) {
        uint32_t next = fat_get(fs, c);
        fat_set(fs, c, 0);
        if (fs->free_count != 0xFFFFFFFF) fs->free_count++;
        if (c < fs->next_free) fs->next_free = c;
        c = next;
    }
}

/* cluster number of the idx-th cluster of a chain, optionally extending it */
static uint32_t chain_at(fat_fs_t *fs, fat_node_t *n, uint32_t *first, uint32_t idx, bool extend, bool zero_new) {
    if (*first < 2) {
        if (!extend) return 0;
        uint32_t c = alloc_cluster(fs, 0, zero_new);
        if (!c) return 0;
        *first = c;
        if (n) { n->last_idx = 0; n->last_clus = c; }
    }
    uint32_t c = *first, i = 0;
    if (n && n->last_clus >= 2 && n->last_idx <= idx) { c = n->last_clus; i = n->last_idx; }
    while (i < idx) {
        uint32_t next = fat_get(fs, c);
        if (next < 2 || next >= EOC) {
            if (!extend) return 0;
            next = alloc_cluster(fs, c, zero_new);
            if (!next) return 0;
        }
        c = next;
        i++;
    }
    if (n) { n->last_idx = idx; n->last_clus = c; }
    return c;
}

/* read or write len bytes at off in the chain starting at *first */
static long chain_rw(fat_fs_t *fs, fat_node_t *n, uint32_t *first, uint64_t off, void *buf, size_t len, bool write,
                     bool zero_new) {
    uint8_t *p = buf;
    size_t done = 0;
    uint8_t sec[512];
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t idx = (uint32_t)(pos / fs->cbytes);
        uint32_t c = chain_at(fs, n, first, idx, write, zero_new);
        if (!c) break;
        uint32_t in_clus = (uint32_t)(pos % fs->cbytes);
        uint32_t lba = clus_lba(fs, c) + in_clus / 512;
        uint32_t in_sec = in_clus % 512;
        size_t chunk;
        if (in_sec == 0 && len - done >= 512) {
            /* whole sectors up to the end of the cluster, extended over physically contiguous clusters */
            uint32_t want = (uint32_t)MIN((len - done) / 512, (size_t)1024);
            uint32_t nsec = MIN(want, (fs->cbytes - in_clus) / 512);
            uint32_t run_clus = c, run_idx = idx;
            while (nsec < want) {
                uint32_t next = chain_at(fs, n, first, run_idx + 1, write, zero_new);
                if (next != run_clus + 1) break;
                run_clus = next;
                run_idx++;
                nsec = MIN(want, nsec + fs->spc);
            }
            chunk = nsec * 512;
            int r = write ? blk_write(fs->dev, lba, nsec, p + done) : blk_read(fs->dev, lba, nsec, p + done);
            if (r < 0) return done ? (long)done : r;
        } else {
            chunk = MIN(512 - in_sec, len - done);
            if (blk_read(fs->dev, lba, 1, sec) < 0) return done ? (long)done : -EIO;
            if (write) {
                memcpy(sec + in_sec, p + done, chunk);
                if (blk_write_lazy(fs->dev, lba, sec) < 0) return done ? (long)done : -EIO;
            } else {
                memcpy(p + done, sec + in_sec, chunk);
            }
        }
        done += chunk;
    }
    return (long)done;
}

/* ------------------------------------------------------------------ names & times */
static uint8_t lfn_checksum(const char *s) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)s[i]);
    return sum;
}

static void short_to_name(const fat_dirent_t *e, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) {
        char c = e->name[i];
        if (i == 0 && c == 0x05) c = (char)0xE5;
        out[n++] = (e->ntres & 0x08) ? (char)tolower((uint8_t)c) : c;
    }
    if (e->name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e->name[i] != ' '; i++)
            out[n++] = (e->ntres & 0x10) ? (char)tolower((uint8_t)e->name[i]) : e->name[i];
    }
    out[n] = 0;
}

static void fat_time(uint64_t t, uint16_t *date, uint16_t *time) {
    uint64_t days = t / 86400, secs = t % 86400;
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    if (y < 1980) y = 1980;
    *date = (uint16_t)(((y - 1980) << 9) | (m << 5) | d);
    *time = (uint16_t)(((secs / 3600) << 11) | ((secs / 60 % 60) << 5) | (secs % 60 / 2));
}

static uint64_t unix_time(uint16_t date, uint16_t time) {
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12 || d < 1) return 0;
    int yy = y - (m <= 2);
    int era = yy / 400;
    int yoe = yy - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (uint64_t)days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
}

/* ------------------------------------------------------------------ directory scanning */
typedef struct {
    fat_dirent_t e;
    uint32_t off;           /* offset of the short entry */
    uint32_t lfn_off;       /* offset of the first LFN entry (== off without LFN) */
    char name[256];
} dirscan_t;

/* iterate valid entries starting at *pos; returns 1 with an entry, 0 at the end */
static int dir_next(fat_fs_t *fs, fat_node_t *dir, uint32_t *first, uint32_t *pos, dirscan_t *out) {
    uint16_t lfn[260];
    int lfn_count = 0;
    uint8_t lfn_sum = 0;
    uint32_t lfn_start = 0;
    bool lfn_ok = false;
    for (;;) {
        fat_dirent_t e;
        long r = chain_rw(fs, dir, first, *pos, &e, 32, false, false);
        if (r < 32) return 0;
        uint32_t here = *pos;
        *pos += 32;
        uint8_t c0 = (uint8_t)e.name[0];
        if (c0 == 0) return 0;
        if (c0 == 0xE5) { lfn_ok = false; continue; }
        if (e.attr == ATTR_LFN) {
            fat_lfn_t *l = (fat_lfn_t *)&e;
            int ord = l->ord & 0x1F;
            if (l->ord & 0x40) {
                lfn_count = ord;
                lfn_sum = l->chksum;
                lfn_start = here;
                lfn_ok = ord > 0 && ord <= 20;
                memset(lfn, 0xFF, sizeof(lfn));
            }
            if (!lfn_ok || ord < 1 || ord > lfn_count || l->chksum != lfn_sum) { lfn_ok = false; continue; }
            int base = (ord - 1) * 13;
            for (int i = 0; i < 5; i++) lfn[base + i] = l->name1[i];
            for (int i = 0; i < 6; i++) lfn[base + 5 + i] = l->name2[i];
            for (int i = 0; i < 2; i++) lfn[base + 11 + i] = l->name3[i];
            continue;
        }
        if (e.attr & ATTR_VOLUME) { lfn_ok = false; continue; }
        out->e = e;
        out->off = here;
        if (lfn_ok && lfn_sum == lfn_checksum(e.name)) {
            int n = 0;
            for (int i = 0; i < lfn_count * 13 && n < 250; i++) {
                uint16_t ch = lfn[i];
                if (ch == 0 || ch == 0xFFFF) break;
                n += utf8_encode(ch, out->name + n);
            }
            out->name[n] = 0;
            out->lfn_off = lfn_start;
        } else {
            short_to_name(&e, out->name);
            out->lfn_off = here;
        }
        return 1;
    }
}

static uint32_t dirent_cluster(const fat_dirent_t *e) { return ((uint32_t)e->clus_hi << 16) | e->clus_lo; }

/* ------------------------------------------------------------------ nodes */
static fat_node_t *node_get(fat_fs_t *fs, uint32_t dir_first, uint32_t off, const fat_dirent_t *e) {
    for (fat_node_t *n = fs->nodes; n; n = n->next) {
        if (!n->is_root && n->dir_first == dir_first && n->dirent_off == off) {
            vnode_ref(n->vn);
            return n;
        }
    }
    fat_node_t *n = kzalloc(sizeof(fat_node_t));
    n->fs = fs;
    n->first = dirent_cluster(e);
    n->dir_first = dir_first;
    n->dirent_off = off;
    n->vn = vnode_alloc((e->attr & ATTR_DIR) ? FT_DIR : FT_FILE, &fat_ops, n);
    n->vn->size = (e->attr & ATTR_DIR) ? 0 : e->size;
    n->vn->mtime = unix_time(e->mdate, e->mtime);
    n->vn->ctime = unix_time(e->cdate, e->ctime);
    n->next = fs->nodes;
    fs->nodes = n;
    return n;
}

static uint32_t dir_first_of(fat_node_t *d) { return d->is_root ? d->fs->root_clus : d->first; }

/* write back size/cluster/time of a node's directory entry */
static int node_sync(fat_node_t *n) {
    if (n->is_root) return 0;
    fat_fs_t *fs = n->fs;
    fat_dirent_t e;
    uint32_t df = n->dir_first;
    if (chain_rw(fs, 0, &df, n->dirent_off, &e, 32, false, false) < 32) return -EIO;
    e.clus_hi = (uint16_t)(n->first >> 16);
    e.clus_lo = (uint16_t)(n->first & 0xFFFF);
    if (n->vn->type == FT_FILE) e.size = (uint32_t)n->vn->size;
    fat_time(n->vn->mtime ? n->vn->mtime : time_now(), &e.mdate, &e.mtime);
    e.adate = e.mdate;
    if (chain_rw(fs, 0, &df, n->dirent_off, &e, 32, true, false) < 32) return -EIO;
    return 0;
}

/* ------------------------------------------------------------------ vnode ops */
static int f_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    fat_node_t *d = dir->priv;
    fat_fs_t *fs = d->fs;
    uint32_t first = dir_first_of(d), pos = 0;
    dirscan_t s;
    while (dir_next(fs, 0, &first, &pos, &s)) {
        if (!strcmp(s.name, ".") || !strcmp(s.name, "..")) continue;
        if (!strcasecmp(s.name, name)) {
            fat_node_t *n = node_get(fs, dir_first_of(d), s.off, &s.e);
            *out = n->vn;
            n->vn->mnt = dir->mnt;
            return 0;
        }
    }
    return -ENOENT;
}

static int f_readdir(vnode_t *dir, uint64_t index, kdirent_t *out) {
    fat_node_t *d = dir->priv;
    fat_fs_t *fs = d->fs;
    uint32_t first = dir_first_of(d);
    uint32_t pos = 0;
    uint64_t i = 0;
    if (index > 0 && d->rd_index == index) { pos = (uint32_t)d->rd_off; i = index; }
    dirscan_t s;
    while (dir_next(fs, 0, &first, &pos, &s)) {
        if (!strcmp(s.name, ".") || !strcmp(s.name, "..")) continue;
        if (i++ == index) {
            memset(out, 0, sizeof(*out));
            out->ino = ((uint64_t)dir_first_of(d) << 32) | s.off;
            out->type = (s.e.attr & ATTR_DIR) ? FT_DIR : FT_FILE;
            out->size = (s.e.attr & ATTR_DIR) ? 0 : s.e.size;
            out->mtime = unix_time(s.e.mdate, s.e.mtime);
            strlcpy(out->name, s.name, sizeof(out->name));
            d->rd_index = index + 1;
            d->rd_off = pos;
            return 1;
        }
    }
    return 0;
}

static long f_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    fat_node_t *node = vn->priv;
    if (off >= vn->size) return 0;
    n = MIN(n, vn->size - off);
    return chain_rw(node->fs, node, &node->first, off, buf, n, false, false);
}

static long f_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) {
    fat_node_t *node = vn->priv;
    if (off + n > 0xFFFFFFFFULL) return -EFBIG;
    /* writing past the end: zero fill the gap */
    if (off > vn->size) {
        uint64_t gap = off - vn->size;
        uint8_t *z = kzalloc(4096);
        uint64_t p = vn->size;
        while (gap) {
            size_t c = MIN(gap, 4096);
            if (chain_rw(node->fs, node, &node->first, p, z, c, true, false) < (long)c) { kfree(z); return -ENOSPC; }
            p += c;
            gap -= c;
        }
        kfree(z);
    }
    long r = chain_rw(node->fs, node, &node->first, off, (void *)buf, n, true, false);
    if (r > 0 && off + r > vn->size) vn->size = off + r;
    vn->mtime = time_now();
    node_sync(node);
    if (r < (long)n && r >= 0) return r ? r : -ENOSPC;
    return r;
}

static int f_truncate(vnode_t *vn, uint64_t size) {
    fat_node_t *n = vn->priv;
    fat_fs_t *fs = n->fs;
    if (size >= vn->size) {
        if (size > vn->size) {
            uint8_t zero = 0;
            f_write(vn, 0, &zero, 1, size - 1);
        }
        return 0;
    }
    uint32_t keep = (uint32_t)((size + fs->cbytes - 1) / fs->cbytes);
    if (keep == 0) {
        free_chain(fs, n->first);
        n->first = 0;
    } else {
        uint32_t last = chain_at(fs, 0, &n->first, keep - 1, false, false);
        if (last) {
            uint32_t rest = fat_get(fs, last);
            fat_set(fs, last, 0x0FFFFFFF);
            if (rest >= 2 && rest < EOC) free_chain(fs, rest);
        }
    }
    n->last_idx = 0;
    n->last_clus = 0;
    vn->size = size;
    vn->mtime = time_now();
    return node_sync(n);
}

static bool valid_short(const char *name) {
    size_t len = strlen(name);
    const char *dot = strchr(name, '.');
    if (dot && strchr(dot + 1, '.')) return false;
    size_t base = dot ? (size_t)(dot - name) : len, ext = dot ? len - base - 1 : 0;
    if (base == 0 || base > 8 || ext > 3) return false;
    for (const char *p = name; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == '.') continue;
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || strchr(" +,;=[]", c)) return false;
    }
    return true;
}

static bool short_exists(fat_fs_t *fs, uint32_t dir_first, const char *sn) {
    uint32_t first = dir_first, pos = 0;
    dirscan_t s;
    while (dir_next(fs, 0, &first, &pos, &s))
        if (!memcmp(s.e.name, sn, 11)) return true;
    return false;
}

static void make_short(fat_fs_t *fs, uint32_t dir_first, const char *name, char sn[11], bool *need_lfn) {
    memset(sn, ' ', 11);
    if (valid_short(name)) {
        const char *dot = strchr(name, '.');
        size_t base = dot ? (size_t)(dot - name) : strlen(name);
        for (size_t i = 0; i < base; i++) sn[i] = name[i];
        if (dot) for (size_t i = 0; dot[1 + i] && i < 3; i++) sn[8 + i] = dot[1 + i];
        *need_lfn = false;
        return;
    }
    *need_lfn = true;
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = 0;
    char base[9] = { 0 }, ext[4] = { 0 };
    int bn = 0, en = 0;
    for (const char *p = name; *p && p != dot && bn < 6; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == ' ' || c == '.') continue;
        if (c >= 0x80 || strchr("+,;=[]", c)) c = '_';
        base[bn++] = (char)toupper(c);
    }
    if (!bn) base[bn++] = '_';
    if (dot)
        for (const char *p = dot + 1; *p && en < 3; p++) {
            uint8_t c = (uint8_t)*p;
            if (c == ' ') continue;
            if (c >= 0x80 || strchr("+,;=[]", c)) c = '_';
            ext[en++] = (char)toupper(c);
        }
    for (int k = 1; k < 1000; k++) {
        char tail[8];
        int tl = snprintf(tail, sizeof(tail), "~%d", k);
        int keep = MIN(bn, 8 - tl);
        memset(sn, ' ', 11);
        memcpy(sn, base, keep);
        memcpy(sn + keep, tail, tl);
        memcpy(sn + 8, ext, en);
        if (!short_exists(fs, dir_first, sn)) return;
    }
}

/* find n consecutive free entry slots in a directory (extending it if needed) */
static uint32_t find_slots(fat_fs_t *fs, uint32_t *dir_first, int n, bool is_root) {
    uint32_t pos = 0, run_start = 0;
    int run = 0;
    for (;;) {
        fat_dirent_t e;
        long r = chain_rw(fs, 0, dir_first, pos, &e, 32, false, false);
        if (r < 32) break;
        uint8_t c0 = (uint8_t)e.name[0];
        if (c0 == 0 || c0 == 0xE5) {
            if (run == 0) run_start = pos;
            if (++run == n) return run_start;
        } else {
            run = 0;
        }
        pos += 32;
    }
    /* extend the directory by one zeroed cluster */
    UNUSED(is_root);
    uint32_t idx = pos / fs->cbytes;
    if (!chain_at(fs, 0, dir_first, idx, true, true)) return 0xFFFFFFFF;
    if (run == 0) run_start = pos;
    /* the slots at the end + the new cluster are all free */
    return run_start;
}

static int write_entries(fat_fs_t *fs, uint32_t dir_first, const char *name, fat_dirent_t *se, bool need_lfn,
                         uint32_t *short_off) {
    uint16_t u16[256];
    int ulen = 0;
    if (need_lfn) {
        const char *p = name;
        while (*p && ulen < 255) {
            uint32_t cp = utf8_decode(&p);
            u16[ulen++] = cp > 0xFFFF ? '_' : (uint16_t)cp;
        }
    }
    int nlfn = need_lfn ? (ulen + 12) / 13 : 0;
    uint32_t df = dir_first;
    uint32_t start = find_slots(fs, &df, nlfn + 1, false);
    if (start == 0xFFFFFFFF) return -ENOSPC;
    uint8_t sum = lfn_checksum(se->name);
    for (int i = 0; i < nlfn; i++) {
        int ord = nlfn - i;
        fat_lfn_t l;
        memset(&l, 0xFF, sizeof(l));
        l.ord = (uint8_t)(ord | (i == 0 ? 0x40 : 0));
        l.attr = ATTR_LFN;
        l.type = 0;
        l.chksum = sum;
        l.zero = 0;
        uint16_t chars[13];
        for (int k = 0; k < 13; k++) {
            int idx = (ord - 1) * 13 + k;
            chars[k] = idx < ulen ? u16[idx] : idx == ulen ? 0 : 0xFFFF;
        }
        memcpy(l.name1, chars, 10);
        memcpy(l.name2, chars + 5, 12);
        memcpy(l.name3, chars + 11, 4);
        if (chain_rw(fs, 0, &df, start + i * 32, &l, 32, true, false) < 32) return -EIO;
    }
    uint32_t soff = start + nlfn * 32;
    if (chain_rw(fs, 0, &df, soff, se, 32, true, false) < 32) return -EIO;
    *short_off = soff;
    return 0;
}

static bool valid_name(const char *name) {
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const char *p = name; *p; p++)
        if (strchr("\\/:*?\"<>|", *p) || (uint8_t)*p < 32) return false;
    return strlen(name) < 250;
}

static int create_entry(fat_node_t *d, const char *name, uint8_t attr, uint32_t cluster, uint32_t size,
                        uint32_t *short_off, fat_dirent_t *out_e) {
    fat_fs_t *fs = d->fs;
    uint32_t df = dir_first_of(d);
    fat_dirent_t se;
    memset(&se, 0, sizeof(se));
    bool need_lfn;
    make_short(fs, df, name, se.name, &need_lfn);
    se.attr = attr;
    se.clus_hi = (uint16_t)(cluster >> 16);
    se.clus_lo = (uint16_t)(cluster & 0xFFFF);
    se.size = size;
    fat_time(time_now(), &se.cdate, &se.ctime);
    se.mdate = se.adate = se.cdate;
    se.mtime = se.ctime;
    int r = write_entries(fs, df, name, &se, need_lfn, short_off);
    if (r == 0 && out_e) *out_e = se;
    return r;
}

static int f_create(vnode_t *dir, const char *name, int type, vnode_t **out) {
    fat_node_t *d = dir->priv;
    fat_fs_t *fs = d->fs;
    if (!valid_name(name)) return -EINVAL;
    vnode_t *exist;
    if (f_lookup(dir, name, &exist) == 0) { vnode_unref(exist); return -EEXIST; }
    uint32_t cluster = 0;
    if (type == FT_DIR) {
        cluster = alloc_cluster(fs, 0, true);
        if (!cluster) return -ENOSPC;
        fat_dirent_t dots[2];
        memset(dots, 0, sizeof(dots));
        memset(dots[0].name, ' ', 11);
        memset(dots[1].name, ' ', 11);
        dots[0].name[0] = '.';
        dots[1].name[0] = dots[1].name[1] = '.';
        dots[0].attr = dots[1].attr = ATTR_DIR;
        dots[0].clus_hi = (uint16_t)(cluster >> 16);
        dots[0].clus_lo = (uint16_t)cluster;
        uint32_t parent = d->is_root ? 0 : d->first;
        dots[1].clus_hi = (uint16_t)(parent >> 16);
        dots[1].clus_lo = (uint16_t)parent;
        fat_time(time_now(), &dots[0].mdate, &dots[0].mtime);
        dots[1].mdate = dots[0].mdate;
        dots[1].mtime = dots[0].mtime;
        uint32_t c = cluster;
        chain_rw(fs, 0, &c, 0, dots, sizeof(dots), true, false);
    }
    uint32_t off;
    fat_dirent_t se;
    int r = create_entry(d, name, type == FT_DIR ? ATTR_DIR : ATTR_ARCHIVE, cluster, 0, &off, &se);
    if (r < 0) {
        if (cluster) free_chain(fs, cluster);
        return r;
    }
    fat_node_t *n = node_get(fs, dir_first_of(d), off, &se);
    n->vn->mnt = dir->mnt;
    dir->mtime = time_now();
    *out = n->vn;
    return 0;
}

/* locate an entry by name: fills scan data */
static int find_entry(fat_node_t *d, const char *name, dirscan_t *out) {
    uint32_t first = dir_first_of(d), pos = 0;
    while (dir_next(d->fs, 0, &first, &pos, out)) {
        if (!strcmp(out->name, ".") || !strcmp(out->name, "..")) continue;
        if (!strcasecmp(out->name, name)) return 0;
    }
    return -ENOENT;
}

static int delete_entries(fat_fs_t *fs, uint32_t dir_first, uint32_t from, uint32_t to) {
    uint32_t df = dir_first;
    for (uint32_t off = from; off <= to; off += 32) {
        uint8_t mark = 0xE5;
        if (chain_rw(fs, 0, &df, off, &mark, 1, true, false) < 1) return -EIO;
    }
    return 0;
}

static bool dir_empty(fat_fs_t *fs, uint32_t first) {
    uint32_t f = first, pos = 0;
    dirscan_t s;
    while (dir_next(fs, 0, &f, &pos, &s))
        if (strcmp(s.name, ".") && strcmp(s.name, "..")) return false;
    return true;
}

static void forget_node(fat_fs_t *fs, uint32_t dir_first, uint32_t off, uint32_t new_dir_first, uint32_t new_off, bool dead) {
    for (fat_node_t *n = fs->nodes; n; n = n->next) {
        if (n->is_root || n->dir_first != dir_first || n->dirent_off != off) continue;
        if (dead) { n->dir_first = 0xFFFFFFFF; n->dirent_off = 0xFFFFFFFF; }
        else { n->dir_first = new_dir_first; n->dirent_off = new_off; }
    }
}

static int f_unlink(vnode_t *dir, const char *name, bool dir_only) {
    fat_node_t *d = dir->priv;
    fat_fs_t *fs = d->fs;
    dirscan_t s;
    if (find_entry(d, name, &s) < 0) return -ENOENT;
    bool is_dir = s.e.attr & ATTR_DIR;
    if (dir_only && !is_dir) return -ENOTDIR;
    if (!dir_only && is_dir) return -EISDIR;
    uint32_t c = dirent_cluster(&s.e);
    if (is_dir && c && !dir_empty(fs, c)) return -ENOTEMPTY;
    int r = delete_entries(fs, dir_first_of(d), s.lfn_off, s.off);
    if (r < 0) return r;
    /* open files keep working on their clusters until closed; we free right away (simple model) */
    free_chain(fs, c);
    forget_node(fs, dir_first_of(d), s.off, 0, 0, true);
    if (c)
        for (fat_node_t *n = fs->nodes; n; n = n->next)
            if (n->first == c) { n->first = 0; n->vn->size = 0; }
    dir->mtime = time_now();
    return 0;
}

static int f_rename(vnode_t *odir, const char *oname, vnode_t *ndir, const char *nname) {
    fat_node_t *od = odir->priv, *nd = ndir->priv;
    fat_fs_t *fs = od->fs;
    if (!valid_name(nname)) return -EINVAL;
    dirscan_t s;
    if (find_entry(od, oname, &s) < 0) return -ENOENT;
    bool is_dir = s.e.attr & ATTR_DIR;
    dirscan_t t;
    if (find_entry(nd, nname, &t) == 0) {
        if (dir_first_of(od) == dir_first_of(nd) && t.off == s.off) {
            /* same entry: only the case changes */
        } else {
            bool tdir = t.e.attr & ATTR_DIR;
            if (tdir != is_dir) return tdir ? -EISDIR : -ENOTDIR;
            int r = f_unlink(ndir, nname, tdir);
            if (r < 0) return r;
        }
    }
    uint32_t off;
    fat_dirent_t se;
    int r = create_entry(nd, nname, s.e.attr, dirent_cluster(&s.e), s.e.size, &off, &se);
    if (r < 0) return r;
    /* re-scan the old entry: creating the new one may not move it, but offsets are stable */
    r = delete_entries(fs, dir_first_of(od), s.lfn_off, s.off);
    if (r < 0) return r;
    forget_node(fs, dir_first_of(od), s.off, dir_first_of(nd), off, false);
    if (is_dir && dir_first_of(od) != dir_first_of(nd)) {
        /* update ".." of the moved directory */
        uint32_t c = dirent_cluster(&s.e);
        fat_dirent_t dotdot;
        if (chain_rw(fs, 0, &c, 32, &dotdot, 32, false, false) == 32) {
            uint32_t parent = nd->is_root ? 0 : nd->first;
            dotdot.clus_hi = (uint16_t)(parent >> 16);
            dotdot.clus_lo = (uint16_t)parent;
            chain_rw(fs, 0, &c, 32, &dotdot, 32, true, false);
        }
    }
    odir->mtime = ndir->mtime = time_now();
    return 0;
}

static void f_release(vnode_t *vn) {
    fat_node_t *n = vn->priv;
    fat_fs_t *fs = n->fs;
    if (n->is_root) { vn->refcount = 1; return; }
    for (fat_node_t **pp = &fs->nodes; *pp; pp = &(*pp)->next) {
        if (*pp == n) { *pp = n->next; break; }
    }
    kfree(n);
    kfree(vn);
}

static const vnode_ops_t fat_ops = {
    .lookup = f_lookup, .create = f_create, .unlink = f_unlink, .rename = f_rename, .readdir = f_readdir,
    .read = f_read, .write = f_write, .truncate = f_truncate, .release = f_release,
};

/* ------------------------------------------------------------------ fs level */
static void write_fsinfo(fat_fs_t *fs) {
    if (!fs->fsinfo || fs->fsinfo == 0xFFFF) return;
    uint8_t sec[512];
    if (blk_read(fs->dev, fs->fsinfo, 1, sec) < 0) return;
    if (*(uint32_t *)sec != 0x41615252) return;
    *(uint32_t *)(sec + 488) = fs->free_count;
    *(uint32_t *)(sec + 492) = fs->next_free;
    blk_write_lazy(fs->dev, fs->fsinfo, sec);
}

static int fat_statfs(mount_t *m, kstatfs_t *st) {
    fat_fs_t *fs = m->priv;
    st->block_size = fs->cbytes;
    st->total_bytes = (uint64_t)fs->nclusters * fs->cbytes;
    if (fs->free_count == 0xFFFFFFFF) {
        uint32_t n = 0;
        for (uint32_t c = 2; c < fs->nclusters + 2; c++) if (fat_get(fs, c) == 0) n++;
        fs->free_count = n;
    }
    st->free_bytes = (uint64_t)fs->free_count * fs->cbytes;
    return 0;
}

static int fat_sync(mount_t *m) {
    fat_fs_t *fs = m->priv;
    write_fsinfo(fs);
    return blk_flush(fs->dev);
}

static const fs_ops_t fat_fs_ops = { .statfs = fat_statfs, .sync = fat_sync };

static mount_t *fat_mounts[8];
static int nfat_mounts;

void fs_sync_all(void) {
    for (int i = 0; i < nfat_mounts; i++) {
        mutex_lock(&fat_mounts[i]->lock);
        fat_sync(fat_mounts[i]);
        mutex_unlock(&fat_mounts[i]->lock);
    }
}

int fat_mount(blkdev_t *dev, const char *path) {
    uint8_t *bs = kmalloc(512);
    if (blk_read(dev, 0, 1, bs) < 0) { kfree(bs); return -EIO; }
    uint16_t bps = bs[11] | (bs[12] << 8);
    uint8_t spc = bs[13];
    uint16_t reserved = bs[14] | (bs[15] << 8);
    uint8_t nfats = bs[16];
    uint16_t root_ents = bs[17] | (bs[18] << 8);
    uint16_t fatsz16 = bs[22] | (bs[23] << 8);
    uint32_t tot16 = bs[19] | (bs[20] << 8);
    uint32_t tot32 = *(uint32_t *)(bs + 32);
    uint32_t fatsz32 = *(uint32_t *)(bs + 36);
    uint32_t root_clus = *(uint32_t *)(bs + 44);
    uint16_t fsinfo = bs[48] | (bs[49] << 8);
    bool sig = bs[510] == 0x55 && bs[511] == 0xAA;
    if (!sig || bps != 512 || !spc || (spc & (spc - 1)) || !nfats || root_ents || fatsz16 || !fatsz32 || root_clus < 2) {
        kfree(bs);
        return -EINVAL;
    }
    fat_fs_t *fs = kzalloc(sizeof(fat_fs_t));
    fs->dev = dev;
    fs->spc = spc;
    fs->reserved = reserved;
    fs->nfats = nfats;
    fs->fat_size = fatsz32;
    fs->root_clus = root_clus;
    fs->total_sectors = tot16 ? tot16 : tot32;
    fs->data_start = reserved + nfats * fatsz32;
    fs->nclusters = (fs->total_sectors - fs->data_start) / spc;
    fs->cbytes = spc * 512;
    fs->fsinfo = fsinfo;
    fs->free_count = 0xFFFFFFFF;
    fs->next_free = 2;
    memcpy(fs->label, bs + 71, 11);
    fs->label[11] = 0;
    for (int i = 10; i >= 0 && fs->label[i] == ' '; i--) fs->label[i] = 0;
    kfree(bs);
    if (fs->total_sectors > dev->nsectors || fs->nclusters < 65525 / 16) { kfree(fs); return -EINVAL; }
    uint8_t sec[512];
    if (fsinfo && fsinfo != 0xFFFF && blk_read(dev, fsinfo, 1, sec) == 0 && *(uint32_t *)sec == 0x41615252) {
        uint32_t fc = *(uint32_t *)(sec + 488), nf = *(uint32_t *)(sec + 492);
        if (fc <= fs->nclusters) fs->free_count = fc;
        if (nf >= 2 && nf < fs->nclusters + 2) fs->next_free = nf;
    }
    fat_node_t *root = kzalloc(sizeof(fat_node_t));
    root->fs = fs;
    root->is_root = true;
    root->first = root_clus;
    root->vn = vnode_alloc(FT_DIR, &fat_ops, root);
    char devname[32];
    snprintf(devname, sizeof(devname), "%s", dev->name);
    int r = vfs_mount(path, root->vn, "fat32", devname, &fat_fs_ops, fs);
    if (r < 0) { kfree(root); kfree(fs); return r; }
    if (nfat_mounts < 8) fat_mounts[nfat_mounts++] = root->vn->mnt;
    klog("[fat] %s mounted on %s: %u clusters of %u bytes, label '%s'\n", dev->name, path, fs->nclusters, fs->cbytes,
         fs->label);
    return 0;
}

/* create an empty FAT32 file system on a whole device (no partition table) */
int fat_mkfs(blkdev_t *dev, const char *label) {
    uint64_t total = dev->nsectors;
    if (total < 66600) return -EINVAL;                  /* too small for FAT32 */
    if (total > 0xFFFFFFFFULL) total = 0xFFFFFFFFULL;
    uint32_t spc = total < 532480 ? 1 : total < 16777216 ? 8 : total < 33554432 ? 16 : 32;
    uint32_t reserved = 32, nfats = 2;
    uint64_t tmp1 = total - reserved, tmp2 = (256 * spc + nfats) / 2;
    uint32_t fatsz = (uint32_t)((tmp1 + tmp2 - 1) / tmp2);
    uint8_t *bs = kzalloc(512);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "CLAUDEOS", 8);
    bs[11] = 0x00; bs[12] = 0x02;
    bs[13] = (uint8_t)spc;
    bs[14] = (uint8_t)reserved; bs[15] = 0;
    bs[16] = (uint8_t)nfats;
    bs[21] = 0xF8;
    bs[24] = 63; bs[26] = 255;
    *(uint32_t *)(bs + 32) = (uint32_t)total;
    *(uint32_t *)(bs + 36) = fatsz;
    *(uint32_t *)(bs + 44) = 2;
    bs[48] = 1;
    bs[50] = 6;
    bs[64] = 0x80;
    bs[66] = 0x29;
    *(uint32_t *)(bs + 67) = (uint32_t)time_now();
    char lab[12];
    memset(lab, ' ', 11);
    for (int i = 0; i < 11 && label && label[i]; i++) lab[i] = (char)toupper((uint8_t)label[i]);
    memcpy(bs + 71, lab, 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    int r = blk_write(dev, 0, 1, bs);
    if (r == 0) r = blk_write(dev, 6, 1, bs);
    /* FSInfo */
    uint8_t *fi = kzalloc(512);
    *(uint32_t *)fi = 0x41615252;
    *(uint32_t *)(fi + 484) = 0x61417272;
    uint32_t nclusters = (uint32_t)((total - reserved - nfats * fatsz) / spc);
    *(uint32_t *)(fi + 488) = nclusters - 1;
    *(uint32_t *)(fi + 492) = 3;
    fi[510] = 0x55; fi[511] = 0xAA;
    if (r == 0) r = blk_write(dev, 1, 1, fi);
    if (r == 0) r = blk_write(dev, 7, 1, fi);
    kfree(fi);
    /* FATs */
    uint8_t *zero = kzalloc(64 * 512);
    for (uint32_t f = 0; f < nfats && r == 0; f++) {
        uint32_t base = reserved + f * fatsz;
        for (uint32_t s = 0; s < fatsz && r == 0; s += 64) r = blk_write(dev, base + s, MIN(64u, fatsz - s), zero);
        uint32_t first[3] = { 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF };
        uint8_t sec[512];
        memset(sec, 0, 512);
        memcpy(sec, first, 12);
        if (r == 0) r = blk_write(dev, base, 1, sec);
    }
    /* root directory cluster with the volume label */
    uint32_t data = reserved + nfats * fatsz;
    memset(zero, 0, 512);
    fat_dirent_t *vol = (fat_dirent_t *)zero;
    memcpy(vol->name, lab, 11);
    vol->attr = ATTR_VOLUME;
    fat_time(time_now(), &vol->mdate, &vol->mtime);
    for (uint32_t s = 0; s < spc && r == 0; s++) r = blk_write(dev, data + s, 1, s == 0 ? zero : zero + 512);
    kfree(zero);
    kfree(bs);
    blk_flush(dev);
    klog("[fat] formatted %s: %u clusters\n", dev->name, nclusters);
    return r;
}
