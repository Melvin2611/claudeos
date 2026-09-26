/* btrfs: single-device file systems (SINGLE/DUP profiles).
 *
 * Reading: chunk mapping, B-tree search, directories, subvolumes, inline/regular/prealloc
 * extents, zlib/lzo/zstd compression.
 * Writing: copy-on-write B-tree updates collected in a transaction and committed together
 * (extent tree, block group tree, free space tree, checksum tree, root items, superblock).
 * Shared blocks (snapshots, reflinks) are never modified: such writes fail with EROFS. */
#include <kernel.h>
#include <vfs.h>
#include <blk.h>
#include <mm.h>
#include "btrfs.h"

/* ------------------------------------------------------------------ crc32c */

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78 : c >> 1;
        crc_table[i] = c;
    }
}

/* raw update (no pre/post inversion), as the kernel's crc32c() */
uint32_t btrfs_crc32c(uint32_t crc, const void *data, size_t n) {
    const uint8_t *p = data;
    if (!crc_table[1]) crc_init();
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

static uint32_t csum_block(const void *data, size_t n) { return ~btrfs_crc32c(~0u, data, n); }
static uint64_t name_hash(const char *name, size_t len) { return btrfs_crc32c(~1u, name, len); }
uint32_t btrfs_csum_data(const void *data, size_t n) { return csum_block(data, n); }
uint64_t btrfs_name_hash(const char *name, size_t len) { return name_hash(name, len); }

uint64_t btrfs_data_ref_hash(uint64_t root, uint64_t owner, uint64_t offset) {
    uint32_t high = btrfs_crc32c(~0u, &root, 8);
    uint32_t low = btrfs_crc32c(~0u, &owner, 8);
    low = btrfs_crc32c(low, &offset, 8);
    return ((uint64_t)high << 31) ^ (uint64_t)low;
}

/* ------------------------------------------------------------------ keys and blocks */

int bkey_cmp(const bkey_t *a, const bkey_t *b) {
    if (a->objectid != b->objectid) return a->objectid < b->objectid ? -1 : 1;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    return 0;
}

/* ------------------------------------------------------------------ chunk mapping */

static chunk_t *chunk_find(btrfs_t *fs, uint64_t logical) {
    for (int i = 0; i < fs->nchunks; i++) {
        chunk_t *c = &fs->chunks[i];
        if (logical >= c->logical && logical < c->logical + c->length) return c;
    }
    return 0;
}

int btrfs_chunk_add(btrfs_t *fs, uint64_t logical, const uint8_t *ci);
static int chunk_add(btrfs_t *fs, uint64_t logical, const uint8_t *ci) { return btrfs_chunk_add(fs, logical, ci); }
int btrfs_chunk_add(btrfs_t *fs, uint64_t logical, const uint8_t *ci) {
    if (chunk_find(fs, logical)) return 0;
    if (fs->nchunks == fs->cap_chunks) {
        int cap = fs->cap_chunks ? fs->cap_chunks * 2 : 32;
        chunk_t *n = kzalloc(cap * sizeof(chunk_t));
        if (fs->chunks) { memcpy(n, fs->chunks, fs->nchunks * sizeof(chunk_t)); kfree(fs->chunks); }
        fs->chunks = n;
        fs->cap_chunks = cap;
    }
    chunk_t *c = &fs->chunks[fs->nchunks];
    c->logical = logical;
    c->length = rd64le(ci + 0);
    c->type = rd64le(ci + 24);
    c->num_stripes = rd16le(ci + 44);
    if (c->num_stripes < 1) return -EINVAL;
    for (int s = 0; s < c->num_stripes && s < 4; s++) {
        c->stripe_dev[s] = rd64le(ci + 48 + s * 32);
        c->stripe_off[s] = rd64le(ci + 48 + s * 32 + 8);
    }
    uint64_t profile = c->type & (BG_RAID0 | BG_RAID1 | BG_RAID10 | BG_RAID5 | BG_RAID6 | BG_RAID1C3 | BG_RAID1C4);
    if (profile) return -EINVAL;             /* multi-device profiles */
    fs->nchunks++;
    return 0;
}

/* physical byte offset of a logical address (first copy) */
static uint64_t map_logical(btrfs_t *fs, uint64_t logical, uint64_t *contig) {
    chunk_t *c = chunk_find(fs, logical);
    if (!c) return (uint64_t)-1;
    if (contig) *contig = c->logical + c->length - logical;
    return c->stripe_off[0] + (logical - c->logical);
}

static int read_phys(btrfs_t *fs, uint64_t phys, void *buf, size_t len) {
    /* the block layer works in 512-byte sectors */
    if ((phys | len) & 511) {
        uint64_t s = phys & ~511ULL, e = ALIGN_UP(phys + len, 512);
        uint8_t *tmp = kmalloc(e - s);
        int r = blk_read(fs->dev, s / 512, (uint32_t)((e - s) / 512), tmp);
        if (r == 0) memcpy(buf, tmp + (phys - s), len);
        kfree(tmp);
        return r;
    }
    return blk_read(fs->dev, phys / 512, (uint32_t)(len / 512), buf);
}

static int write_phys(btrfs_t *fs, uint64_t phys, const void *buf, size_t len) {
    if ((phys | len) & 511) return -EINVAL;
    return blk_write(fs->dev, phys / 512, (uint32_t)(len / 512), buf);
}

int btrfs_read_logical(btrfs_t *fs, uint64_t logical, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        uint64_t contig;
        uint64_t phys = map_logical(fs, logical, &contig);
        if (phys == (uint64_t)-1) return -EIO;
        size_t n = MIN(len, contig);
        int r = read_phys(fs, phys, p, n);
        if (r < 0) return r;
        p += n;
        logical += n;
        len -= n;
    }
    return 0;
}

/* write all copies (DUP stores two) */
int btrfs_write_logical(btrfs_t *fs, uint64_t logical, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        chunk_t *c = chunk_find(fs, logical);
        if (!c) return -EIO;
        size_t n = MIN(len, c->logical + c->length - logical);
        for (int s = 0; s < c->num_stripes && s < 4; s++) {
            int r = write_phys(fs, c->stripe_off[s] + (logical - c->logical), p, n);
            if (r < 0) return r;
        }
        p += n;
        logical += n;
        len -= n;
    }
    return 0;
}

/* ------------------------------------------------------------------ tree block cache */

#define EB_HASH 256

ebuf_t *btrfs_eb_lookup(btrfs_t *fs, uint64_t bytenr) {
    for (ebuf_t *e = fs->eb_hash[(bytenr / fs->nodesize) % EB_HASH]; e; e = e->hnext)
        if (e->bytenr == bytenr) return e;
    return 0;
}

void btrfs_eb_insert(btrfs_t *fs, ebuf_t *e) {
    int h = (e->bytenr / fs->nodesize) % EB_HASH;
    e->hnext = fs->eb_hash[h];
    fs->eb_hash[h] = e;
    fs->eb_count++;
}

void btrfs_eb_remove(btrfs_t *fs, ebuf_t *e) {
    for (ebuf_t **pp = &fs->eb_hash[(e->bytenr / fs->nodesize) % EB_HASH]; *pp; pp = &(*pp)->hnext) {
        if (*pp == e) { *pp = e->hnext; fs->eb_count--; return; }
    }
}

/* drop clean, unreferenced buffers when the cache grows too large */
static void eb_trim(btrfs_t *fs) {
    if (fs->eb_count < 1024) return;
    uint64_t cutoff = fs->eb_clock > 512 ? fs->eb_clock - 512 : 0;
    for (int h = 0; h < EB_HASH; h++) {
        for (ebuf_t **pp = &fs->eb_hash[h]; *pp;) {
            ebuf_t *e = *pp;
            if (!e->dirty && !e->refs && e->used < cutoff) {
                *pp = e->hnext;
                fs->eb_count--;
                kfree(e->data);
                kfree(e);
            } else {
                pp = &e->hnext;
            }
        }
    }
}

ebuf_t *btrfs_read_block(btrfs_t *fs, uint64_t bytenr) {
    ebuf_t *e = btrfs_eb_lookup(fs, bytenr);
    if (e) {
        e->used = ++fs->eb_clock;
        e->refs++;
        return e;
    }
    eb_trim(fs);
    e = kzalloc(sizeof(ebuf_t));
    e->data = kmalloc(fs->nodesize);
    e->bytenr = bytenr;
    if (btrfs_read_logical(fs, bytenr, e->data, fs->nodesize) < 0 || rd64le(e->data + H_BYTENR) != bytenr) {
        klog("[btrfs] bad tree block at %lu\n", bytenr);
        kfree(e->data);
        kfree(e);
        return 0;
    }
    if (fs->csum_type == 0 && csum_block(e->data + 32, fs->nodesize - 32) != rd32le(e->data)) {
        klog("[btrfs] checksum mismatch in tree block %lu\n", bytenr);
        kfree(e->data);
        kfree(e);
        return 0;
    }
    e->used = ++fs->eb_clock;
    e->refs = 1;
    btrfs_eb_insert(fs, e);
    return e;
}

void btrfs_put_block(ebuf_t *e) {
    if (!e || e->refs <= 0) return;
    if (--e->refs == 0 && e->orphan) {
        kfree(e->data);
        kfree(e);
    }
}

/* ------------------------------------------------------------------ paths and search */

void path_release(path_t *p) {
    for (int i = 0; i < BTRFS_MAX_LEVEL; i++) {
        btrfs_put_block(p->nodes[i]);
        p->nodes[i] = 0;
        p->slots[i] = 0;
    }
}

/* binary search in a block: returns the first slot with key >= k, *found if equal */
int btrfs_bin_search(ebuf_t *b, const bkey_t *k, bool *found) {
    int lo = 0, hi = (int)nritems(b);
    bool leaf = blevel(b) == 0;
    *found = false;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        bkey_t mk;
        if (leaf) item_key(b, mid, &mk); else ptr_key(b, mid, &mk);
        int c = bkey_cmp(&mk, k);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid;
        else { *found = true; return mid; }
    }
    return lo;
}

/* read-only search: 0 = found, 1 = not found (slot = insertion point), <0 error */
int btrfs_search(btrfs_t *fs, btree_t *t, const bkey_t *key, path_t *p) {
    return btrfs_search_cow(fs, t, key, p, 0, false);
}

/* move to the next leaf item; 0 = ok, 1 = end of tree */
int btrfs_next_item(btrfs_t *fs, path_t *p) {
    ebuf_t *leaf = p->nodes[0];
    if (++p->slots[0] < (int)nritems(leaf)) return 0;
    int level = 1;
    for (; level < BTRFS_MAX_LEVEL; level++) {
        if (!p->nodes[level]) return 1;
        if (p->slots[level] + 1 < (int)nritems(p->nodes[level])) break;
    }
    if (level == BTRFS_MAX_LEVEL) return 1;
    p->slots[level]++;
    for (int l = level; l > 0; l--) {
        ebuf_t *child = btrfs_read_block(fs, ptr_block(p->nodes[l], p->slots[l]));
        if (!child) return -EIO;
        btrfs_put_block(p->nodes[l - 1]);
        p->nodes[l - 1] = child;
        p->slots[l - 1] = 0;
    }
    return nritems(p->nodes[0]) ? 0 : btrfs_next_item(fs, p);
}

/* position the path at the first item >= key; returns 1 at the end of the tree */
int btrfs_seek(btrfs_t *fs, btree_t *t, const bkey_t *key, path_t *p) {
    int r = btrfs_search(fs, t, key, p);
    if (r < 0) return r;
    if (p->slots[0] >= (int)nritems(p->nodes[0])) {
        p->slots[0]--;
        return btrfs_next_item(fs, p);
    }
    return 0;
}

void btrfs_path_key(path_t *p, bkey_t *k) { item_key(p->nodes[0], p->slots[0], k); }
uint8_t *btrfs_path_data(path_t *p, uint32_t *size) {
    if (size) *size = item_size(p->nodes[0], p->slots[0]);
    return item_data(p->nodes[0], p->slots[0]);
}

/* copy one item out of a tree (exact key); returns its size or <0 */
int btrfs_lookup_item(btrfs_t *fs, btree_t *t, const bkey_t *key, void *buf, uint32_t len) {
    path_t p = { 0 };
    int r = btrfs_search(fs, t, key, &p);
    if (r) { path_release(&p); return r < 0 ? r : -ENOENT; }
    uint32_t sz;
    uint8_t *d = btrfs_path_data(&p, &sz);
    memcpy(buf, d, MIN(sz, len));
    path_release(&p);
    return (int)sz;
}

/* ------------------------------------------------------------------ trees */

btree_t *btrfs_get_tree(btrfs_t *fs, uint64_t id) {
    for (btree_t *t = fs->trees; t; t = t->next)
        if (t->id == id) return t;
    /* find the root item in the root tree (the highest offset for this objectid) */
    bkey_t k = { id, BTRFS_ROOT_ITEM_KEY, (uint64_t)-1 };
    path_t p = { 0 };
    int r = btrfs_search(fs, fs->root_tree, &k, &p);
    if (r < 0) { path_release(&p); return 0; }
    if (p.slots[0] > 0) p.slots[0]--;
    else { path_release(&p); return 0; }
    bkey_t found;
    btrfs_path_key(&p, &found);
    if (found.objectid != id || found.type != BTRFS_ROOT_ITEM_KEY) { path_release(&p); return 0; }
    uint32_t sz;
    uint8_t *ri = btrfs_path_data(&p, &sz);
    btree_t *t = kzalloc(sizeof(btree_t));
    t->id = id;
    t->root_key = found;
    t->bytenr = rd64le(ri + RI_BYTENR);
    t->level = ri[RI_LEVEL];
    t->root_item_size = MIN(sz, (uint32_t)sizeof(t->root_item));
    memcpy(t->root_item, ri, t->root_item_size);
    path_release(&p);
    t->next = fs->trees;
    fs->trees = t;
    return t;
}

/* ------------------------------------------------------------------ superblock + mount */

static const vnode_ops_t btrfs_vops;
static const fs_ops_t btrfs_fsops;

static int load_chunks(btrfs_t *fs) {
    /* bootstrap: system chunks from the superblock */
    uint32_t n = rd32le(fs->super + SB_SYS_CHUNK_SIZE);
    const uint8_t *a = fs->super + SB_SYS_CHUNK_ARRAY;
    for (uint32_t off = 0; off + 17 + 48 <= n && off < 2048;) {
        bkey_t k;
        read_key(a + off, &k);
        const uint8_t *ci = a + off + 17;
        uint16_t ns = rd16le(ci + 44);
        if (k.type != BTRFS_CHUNK_ITEM_KEY || chunk_add(fs, k.offset, ci) < 0) return -EINVAL;
        off += 17 + 48 + ns * 32;
    }
    /* the chunk tree has all chunks */
    fs->chunk_tree = kzalloc(sizeof(btree_t));
    fs->chunk_tree->id = BTRFS_CHUNK_TREE_OBJECTID;
    fs->chunk_tree->bytenr = rd64le(fs->super + SB_CHUNK_ROOT);
    fs->chunk_tree->level = fs->super[SB_CHUNK_ROOT_LEVEL];
    ebuf_t *cr = btrfs_read_block(fs, fs->chunk_tree->bytenr);
    if (!cr) return -EIO;
    memcpy(fs->chunk_uuid, cr->data + H_CHUNK_UUID, 16);
    btrfs_put_block(cr);
    bkey_t k = { BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_CHUNK_ITEM_KEY, 0 };
    path_t p = { 0 };
    int r = btrfs_seek(fs, fs->chunk_tree, &k, &p);
    while (r == 0) {
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        if (ik.type == BTRFS_CHUNK_ITEM_KEY) {
            if (chunk_add(fs, ik.offset, btrfs_path_data(&p, 0)) < 0) { path_release(&p); return -EINVAL; }
        }
        r = btrfs_next_item(fs, &p);
    }
    path_release(&p);
    return r < 0 ? r : 0;
}

static btrfs_node_t *node_get(btrfs_t *fs, btree_t *root, uint64_t ino);

int btrfs_mount(blkdev_t *dev, const char *path) {
    uint8_t *sb = kmalloc(4096);
    if (blk_read(dev, BTRFS_SUPER_OFFSET / 512, 8, sb) < 0 || rd64le(sb + SB_MAGIC) != BTRFS_MAGIC) {
        kfree(sb);
        return -EINVAL;
    }
    btrfs_t *fs = kzalloc(sizeof(btrfs_t));
    fs->dev = dev;
    fs->super = sb;
    fs->nodesize = rd32le(sb + SB_NODESIZE);
    fs->sectorsize = rd32le(sb + SB_SECTORSIZE);
    fs->csum_type = rd16le(sb + SB_CSUM_TYPE);
    fs->incompat = rd64le(sb + SB_INCOMPAT);
    fs->compat_ro = rd64le(sb + SB_COMPAT_RO);
    fs->generation = rd64le(sb + SB_GENERATION);
    memcpy(fs->fsid, sb + SB_FSID, 16);
    memcpy(fs->meta_uuid, (fs->incompat & INCOMPAT_METADATA_UUID) ? sb + SB_METADATA_UUID : sb + SB_FSID, 16);
    memcpy(fs->label, sb + SB_LABEL, 32);
    fs->label[31] = 0;
    uint64_t ndev = rd64le(sb + SB_NUM_DEVICES);
    if (fs->nodesize < 4096 || fs->nodesize > 65536 || fs->sectorsize < 4096 || ndev != 1 ||
        csum_block(sb + 32, 4096 - 32) != rd32le(sb)) {
        klog("[btrfs] %s: unsupported (devices %lu, nodesize %u) or damaged superblock\n", dev->name, ndev,
             fs->nodesize);
        kfree(sb);
        kfree(fs);
        return -EINVAL;
    }
    if (load_chunks(fs) < 0) {
        klog("[btrfs] %s: cannot read the chunk tree (multi-device profile?)\n", dev->name);
        kfree(fs->chunks);
        kfree(sb);
        kfree(fs);
        return -EINVAL;
    }
    fs->root_tree = kzalloc(sizeof(btree_t));
    fs->root_tree->id = BTRFS_ROOT_TREE_OBJECTID;
    fs->root_tree->bytenr = rd64le(sb + SB_ROOT);
    fs->root_tree->level = sb[SB_ROOT_LEVEL];
    btree_t *fst = btrfs_get_tree(fs, BTRFS_FS_TREE_OBJECTID);
    if (!fst) {
        klog("[btrfs] %s: no file system tree\n", dev->name);
        return -EINVAL;
    }
    fs->rw = btrfs_can_write(fs);
    btrfs_node_t *root = node_get(fs, fst, BTRFS_FIRST_FREE_OBJECTID);
    if (!root) return -EIO;
    char devname[32];
    strlcpy(devname, dev->name, sizeof(devname));
    int r = vfs_mount(path, root->vn, "btrfs", devname, &btrfs_fsops, fs);
    if (r < 0) return r;
    fs->mnt = root->vn->mnt;
    dev->mounted = true;
    klog("[btrfs] %s mounted on %s: label '%s', generation %lu, %s\n", dev->name, path, fs->label, fs->generation,
         fs->rw ? "read-write" : "read-only");
    return 0;
}

/* ------------------------------------------------------------------ inodes */

static btrfs_node_t *node_get(btrfs_t *fs, btree_t *root, uint64_t ino) {
    for (btrfs_node_t *n = fs->nodes; n; n = n->next) {
        if (n->root == root && n->ino == ino && !n->dead) {
            vnode_ref(n->vn);
            return n;
        }
    }
    uint8_t ii[INODE_ITEM_SIZE];
    bkey_t k = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    if (btrfs_lookup_item(fs, root, &k, ii, sizeof(ii)) < 0) return 0;
    btrfs_node_t *n = kzalloc(sizeof(btrfs_node_t));
    n->fs = fs;
    n->root = root;
    n->ino = ino;
    memcpy(n->inode, ii, sizeof(ii));
    uint32_t mode = rd32le(ii + II_MODE);
    int type = (mode & 0170000) == 0040000 ? FT_DIR : FT_FILE;
    n->vn = vnode_alloc(type, &btrfs_vops, n);
    n->vn->size = rd64le(ii + II_SIZE);
    n->vn->mtime = rd64le(ii + II_MTIME);
    n->vn->ctime = rd64le(ii + II_CTIME);
    n->vn->mnt = fs->mnt;
    n->next = fs->nodes;
    fs->nodes = n;
    return n;
}

static void node_release(vnode_t *vn) {
    btrfs_node_t *n = vn->priv;
    btrfs_t *fs = n->fs;
    if (fs->mnt) mutex_lock(&fs->mnt->lock);
    btrfs_flush_node(n);
    if (fs->mnt) mutex_unlock(&fs->mnt->lock);
    for (btrfs_node_t **pp = &fs->nodes; *pp; pp = &(*pp)->next) {
        if (*pp == n) { *pp = n->next; break; }
    }
    kfree(n->wbuf);
    kfree(n);
    kfree(vn);
}

/* ------------------------------------------------------------------ directories */

static int dir_type(uint8_t t) { return t == BTRFS_FT_DIR ? FT_DIR : FT_FILE; }

/* resolve a directory entry's location key to a node (crossing into subvolumes) */
static btrfs_node_t *entry_node(btrfs_t *fs, btree_t *root, const uint8_t *di) {
    bkey_t loc;
    read_key(di, &loc);
    if (loc.type == BTRFS_ROOT_ITEM_KEY) {
        btree_t *sub = btrfs_get_tree(fs, loc.objectid);
        if (!sub) return 0;
        return node_get(fs, sub, BTRFS_FIRST_FREE_OBJECTID);
    }
    return node_get(fs, root, loc.objectid);
}

/* find a name inside a DIR_ITEM (several names can share a hash) */
uint8_t *btrfs_dir_match(uint8_t *data, uint32_t size, const char *name, size_t len) {
    uint32_t off = 0;
    while (off + DIR_ITEM_SIZE <= size) {
        uint8_t *di = data + off;
        uint16_t dlen = rd16le(di + DI_DATA_LEN), nlen = rd16le(di + DI_NAME_LEN);
        if (nlen == len && !memcmp(di + DIR_ITEM_SIZE, name, len)) return di;
        off += DIR_ITEM_SIZE + nlen + dlen;
    }
    return 0;
}

static int b_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    btrfs_node_t *d = dir->priv;
    btrfs_t *fs = d->fs;
    size_t len = strlen(name);
    bkey_t k = { d->ino, BTRFS_DIR_ITEM_KEY, name_hash(name, len) };
    path_t p = { 0 };
    int r = btrfs_search(fs, d->root, &k, &p);
    if (r != 0) { path_release(&p); return r < 0 ? r : -ENOENT; }
    uint32_t sz;
    uint8_t *data = btrfs_path_data(&p, &sz);
    uint8_t *di = btrfs_dir_match(data, sz, name, len);
    btrfs_node_t *n = di ? entry_node(fs, d->root, di) : 0;
    path_release(&p);
    if (!n) return -ENOENT;
    n->vn->mnt = dir->mnt;
    *out = n->vn;
    return 0;
}

static int b_readdir(vnode_t *dir, uint64_t index, kdirent_t *out) {
    btrfs_node_t *d = dir->priv;
    btrfs_t *fs = d->fs;
    /* continue from the last position when reading sequentially */
    uint64_t start = (index > 0 && d->rd_index == index) ? d->rd_next : 0;
    uint64_t skip = (index > 0 && d->rd_index == index) ? 0 : index;
    bkey_t k = { d->ino, BTRFS_DIR_INDEX_KEY, start };
    path_t p = { 0 };
    int r = btrfs_seek(fs, d->root, &k, &p);
    while (r == 0) {
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        if (ik.objectid != d->ino || ik.type != BTRFS_DIR_INDEX_KEY) break;
        if (skip) { skip--; r = btrfs_next_item(fs, &p); continue; }
        uint8_t *di = btrfs_path_data(&p, 0);
        uint16_t nlen = rd16le(di + DI_NAME_LEN);
        memset(out, 0, sizeof(*out));
        memcpy(out->name, di + DIR_ITEM_SIZE, MIN(nlen, (uint16_t)255));
        out->type = dir_type(di[DI_TYPE]);
        bkey_t loc;
        read_key(di, &loc);
        out->ino = loc.objectid;
        if (loc.type == BTRFS_INODE_ITEM_KEY) {
            uint8_t ii[INODE_ITEM_SIZE];
            bkey_t ikey = { loc.objectid, BTRFS_INODE_ITEM_KEY, 0 };
            if (btrfs_lookup_item(fs, d->root, &ikey, ii, sizeof(ii)) >= 0) {
                if (out->type == FT_FILE) out->size = rd64le(ii + II_SIZE);
                out->mtime = rd64le(ii + II_MTIME);
            }
        }
        d->rd_index = index + 1;
        d->rd_next = ik.offset + 1;
        path_release(&p);
        return 1;
    }
    path_release(&p);
    return r < 0 ? r : 0;
}

/* ------------------------------------------------------------------ file data */

/* read file bytes [off, off+len) from the extents (zeros for holes and past i_size) */
long btrfs_read_range(btrfs_node_t *n, uint64_t off, uint8_t *buf, size_t len) {
    btrfs_t *fs = n->fs;
    uint64_t isize = rd64le(n->inode + II_SIZE);
    memset(buf, 0, len);
    if (off >= isize) return 0;
    size_t want = MIN(len, isize - off);
    uint64_t end = off + want;
    /* start at the extent covering "off": the last item with key offset <= off */
    bkey_t k = { n->ino, BTRFS_EXTENT_DATA_KEY, off };
    path_t p = { 0 };
    int r = btrfs_search(fs, n->root, &k, &p);
    if (r < 0) { path_release(&p); return r; }
    if (r == 1 && p.slots[0] > 0) p.slots[0]--;
    bool done = false;
    if (p.slots[0] >= (int)nritems(p.nodes[0])) done = btrfs_next_item(fs, &p) != 0;
    while (!done) {
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        if (ik.objectid != n->ino || ik.type != BTRFS_EXTENT_DATA_KEY) {
            if (ik.objectid > n->ino || (ik.objectid == n->ino && ik.type > BTRFS_EXTENT_DATA_KEY)) break;
            if (btrfs_next_item(fs, &p)) break;
            continue;
        }
        if (ik.offset >= end) break;
        uint32_t isz;
        uint8_t *fe = btrfs_path_data(&p, &isz);
        uint8_t type = fe[FE_TYPE], comp = fe[FE_COMPRESSION];
        uint64_t ram = rd64le(fe + FE_RAM_BYTES);
        if (type == BTRFS_FILE_EXTENT_INLINE) {
            uint32_t dlen = isz - FE_INLINE_DATA;
            uint8_t *data = fe + FE_INLINE_DATA;
            uint8_t *plain = data;
            if (comp) {
                plain = kzalloc(ram + 1);
                if (btrfs_decompress(comp, data, dlen, plain, ram, fs->sectorsize) < 0) memset(plain, 0, ram);
            }
            uint64_t s = MAX(off, ik.offset), e = MIN(end, ik.offset + ram);
            if (s < e) memcpy(buf + (s - off), plain + (s - ik.offset), e - s);
            if (comp) kfree(plain);
        } else {
            uint64_t disk = rd64le(fe + FE_DISK_BYTENR), dnum = rd64le(fe + FE_DISK_NUM_BYTES);
            uint64_t eoff = rd64le(fe + FE_OFFSET), num = rd64le(fe + FE_NUM_BYTES);
            uint64_t s = MAX(off, ik.offset), e = MIN(end, ik.offset + num);
            if (s < e && disk && type == BTRFS_FILE_EXTENT_REG) {
                if (!comp) {
                    int rr = btrfs_read_logical(fs, disk + eoff + (s - ik.offset), buf + (s - off), e - s);
                    if (rr < 0) { path_release(&p); return rr; }
                } else {
                    uint8_t *cbuf = kmalloc(dnum), *plain = kzalloc(ram + 1);
                    int rr = btrfs_read_logical(fs, disk, cbuf, dnum);
                    if (rr == 0) rr = btrfs_decompress(comp, cbuf, dnum, plain, ram, fs->sectorsize);
                    if (rr >= 0) memcpy(buf + (s - off), plain + eoff + (s - ik.offset), e - s);
                    kfree(cbuf);
                    kfree(plain);
                    if (rr < 0) { path_release(&p); return -EIO; }
                }
            }
        }
        if (btrfs_next_item(fs, &p)) break;
    }
    path_release(&p);
    return (long)want;
}

static long b_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    UNUSED(f);
    btrfs_node_t *node = vn->priv;
    if (vn->type == FT_DIR) return -EISDIR;
    btrfs_flush_node(node);
    return btrfs_read_range(node, off, buf, n);
}

/* ------------------------------------------------------------------ fs ops */

static int b_statfs(mount_t *m, kstatfs_t *st) {
    btrfs_t *fs = m->priv;
    uint64_t total = rd64le(fs->super + SB_TOTAL_BYTES);
    uint64_t used = rd64le(fs->super + SB_BYTES_USED) + fs->bytes_used_delta + fs->data_reserved;
    st->block_size = fs->sectorsize;
    st->total_bytes = total;
    st->free_bytes = total > used ? total - used : 0;
    return 0;
}

static int b_sync(mount_t *m) {
    btrfs_t *fs = m->priv;
    return btrfs_commit(fs);
}

static void b_umount(mount_t *m) {
    btrfs_t *fs = m->priv;
    fs->gone = true;
}

static const fs_ops_t btrfs_fsops = { .statfs = b_statfs, .sync = b_sync, .umount = b_umount };

static const vnode_ops_t btrfs_vops = {
    .lookup = b_lookup, .readdir = b_readdir, .read = b_read, .write = btrfs_vn_write, .create = btrfs_vn_create,
    .unlink = btrfs_vn_unlink, .rename = btrfs_vn_rename, .truncate = btrfs_vn_truncate,
    .close = btrfs_vn_close, .release = node_release,
};

btrfs_node_t *btrfs_node_get(btrfs_t *fs, btree_t *root, uint64_t ino) { return node_get(fs, root, ino); }
