/* btrfs write support: copy-on-write B-tree, allocator, deferred extent references,
 * free space tree, checksums, transaction commit and the write-side vnode operations. */
#include <kernel.h>
#include <vfs.h>
#include <blk.h>
#include <mm.h>
#include "btrfs.h"

bool cmdline_has(const char *opt);

#define WBUF_MAX (1024 * 1024)
#define FSI_USING_BITMAPS 1

static inline uint32_t leaf_data_size(btrfs_t *fs) { return fs->nodesize - HDR_SIZE; }
static inline uint32_t max_ptrs(btrfs_t *fs) { return (fs->nodesize - HDR_SIZE) / PTR_SIZE; }
static inline uint32_t leaf_data_end(btrfs_t *fs, ebuf_t *b) {
    uint32_t n = nritems(b);
    return n ? item_off(b, n - 1) : leaf_data_size(fs);
}
static inline int leaf_free(btrfs_t *fs, ebuf_t *b) { return (int)leaf_data_end(fs, b) - (int)(nritems(b) * ITEM_SIZE); }
static inline uint8_t *ldata(ebuf_t *b) { return b->data + HDR_SIZE; }

static inline void set_item(ebuf_t *b, int i, const bkey_t *k, uint32_t off, uint32_t size) {
    uint8_t *it = leaf_item(b, i);
    write_key(it, k);
    wr32le(it + 17, off);
    wr32le(it + 21, size);
}
static inline void set_item_off(ebuf_t *b, int i, uint32_t off) { wr32le(leaf_item(b, i) + 17, off); }
static inline void set_item_size(ebuf_t *b, int i, uint32_t size) { wr32le(leaf_item(b, i) + 21, size); }

static inline void set_ptr(ebuf_t *b, int i, const bkey_t *k, uint64_t bytenr, uint64_t gen) {
    uint8_t *p = node_ptr(b, i);
    write_key(p, k);
    wr64le(p + 17, bytenr);
    wr64le(p + 25, gen);
}

static int fail(btrfs_t *fs, const char *what, int err) {
    if (!fs->failed) klog("[btrfs] %s failed (%d): further writes disabled\n", what, err);
    fs->failed = true;
    return err;
}

/* ------------------------------------------------------------------ feature check */

bool btrfs_can_write(btrfs_t *fs) {
    if (cmdline_has("btrfs_ro")) return false;
    uint64_t ok_incompat = INCOMPAT_MIXED_BACKREF | INCOMPAT_DEFAULT_SUBVOL | INCOMPAT_COMPRESS_LZO |
                           INCOMPAT_COMPRESS_ZSTD | INCOMPAT_BIG_METADATA | INCOMPAT_EXTENDED_IREF |
                           INCOMPAT_SKINNY_METADATA | INCOMPAT_NO_HOLES;
    uint64_t ok_ro = COMPAT_RO_FREE_SPACE_TREE | COMPAT_RO_FREE_SPACE_TREE_VALID | COMPAT_RO_BLOCK_GROUP_TREE;
    const char *why = 0;
    if (fs->csum_type != 0) why = "checksum type";
    else if (fs->incompat & ~ok_incompat) why = "incompatible features";
    else if (fs->compat_ro & ~ok_ro) why = "read-only features";
    else if ((fs->compat_ro & COMPAT_RO_FREE_SPACE_TREE) && !(fs->compat_ro & COMPAT_RO_FREE_SPACE_TREE_VALID))
        why = "free space tree not valid";
    else if (rd64le(fs->super + SB_LOG_ROOT)) why = "log tree needs replay (unclean shutdown)";
    else if (btrfs_get_tree(fs, BTRFS_QUOTA_TREE_OBJECTID)) why = "quotas enabled";
    else if (!btrfs_get_tree(fs, BTRFS_EXTENT_TREE_OBJECTID)) why = "no extent tree";
    if (why) {
        klog("[btrfs] %s: mounting read-only (%s)\n", fs->dev->name, why);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ block groups and allocation */

static void range_insert(frange_t **list, uint64_t start, uint64_t len) {
    frange_t **pp = list;
    while (*pp && (*pp)->start < start) pp = &(*pp)->next;
    frange_t *prev = 0;
    for (frange_t *r = *list; r && r->start < start; r = r->next) prev = r;
    /* merge with the previous and/or next range */
    if (prev && prev->start + prev->len == start) {
        prev->len += len;
        frange_t *nx = prev->next;
        if (nx && prev->start + prev->len == nx->start) {
            prev->len += nx->len;
            prev->next = nx->next;
            kfree(nx);
        }
        return;
    }
    if (*pp && start + len == (*pp)->start) {
        (*pp)->start = start;
        (*pp)->len += len;
        return;
    }
    frange_t *n = kmalloc(sizeof(frange_t));
    n->start = start;
    n->len = len;
    n->next = *pp;
    *pp = n;
}

/* remove [start, start+len) from a range list (it may cover several ranges) */
static void range_remove(frange_t **list, uint64_t start, uint64_t len) {
    uint64_t end = start + len;
    for (frange_t **pp = list; *pp;) {
        frange_t *r = *pp;
        uint64_t rs = r->start, re = r->start + r->len;
        if (re <= start || rs >= end) { pp = &r->next; continue; }
        if (rs < start && re > end) {
            frange_t *n = kmalloc(sizeof(frange_t));
            n->start = end;
            n->len = re - end;
            n->next = r->next;
            r->len = start - rs;
            r->next = n;
            return;
        }
        if (rs < start) { r->len = start - rs; pp = &r->next; continue; }
        if (re > end) { r->start = end; r->len = re - end; return; }
        *pp = r->next;
        kfree(r);
    }
}

static bgroup_t *bg_find(btrfs_t *fs, uint64_t bytenr) {
    for (bgroup_t *g = fs->bgroups; g; g = g->next)
        if (bytenr >= g->start && bytenr < g->start + g->len) return g;
    return 0;
}

static btree_t *bg_tree(btrfs_t *fs) {
    if (fs->compat_ro & COMPAT_RO_BLOCK_GROUP_TREE) return btrfs_get_tree(fs, BTRFS_BLOCK_GROUP_TREE_OBJECTID);
    return btrfs_get_tree(fs, BTRFS_EXTENT_TREE_OBJECTID);
}

/* superblock mirrors are never allocated (and are not free space); optionally collect them */
static int exclude_supers(btrfs_t *fs, chunk_t *c, uint64_t out[][2]) {
    static const uint64_t supers[3] = { 0x10000ULL, 0x4000000ULL, 0x4000000000ULL };
    int n = 0;
    for (int s = 0; s < c->num_stripes; s++)
        for (int m = 0; m < 3; m++)
            if (supers[m] >= c->stripe_off[s] && supers[m] < c->stripe_off[s] + c->length) {
                uint64_t logical = c->logical + (supers[m] - c->stripe_off[s]);
                uint64_t len = MIN(0x10000ULL, c->logical + c->length - logical);
                bgroup_t *g = bg_find(fs, logical);
                if (g) range_remove(&g->free, logical, len);
                if (out && n < 6) { out[n][0] = logical; out[n][1] = len; }
                n++;
            }
    return n;
}

static int load_space(btrfs_t *fs) {
    if (fs->space_loaded) return 0;
    btree_t *bgt = bg_tree(fs), *ext = btrfs_get_tree(fs, BTRFS_EXTENT_TREE_OBJECTID);
    if (!bgt || !ext) return -EIO;
    bkey_t k = { 0, BTRFS_BLOCK_GROUP_ITEM_KEY, 0 };
    path_t p = { 0 };
    int r = btrfs_seek(fs, bgt, &k, &p);
    bgroup_t **tail = &fs->bgroups;
    while (r == 0) {
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        if (ik.type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
            uint8_t *d = btrfs_path_data(&p, 0);
            bgroup_t *g = kzalloc(sizeof(bgroup_t));
            g->start = ik.objectid;
            g->len = ik.offset;
            g->used = rd64le(d);
            g->flags = rd64le(d + 16);
            range_insert(&g->free, g->start, g->len);
            *tail = g;
            tail = &g->next;
        }
        r = btrfs_next_item(fs, &p);
    }
    path_release(&p);
    if (r < 0) return r;
    for (int i = 0; i < fs->nchunks; i++) exclude_supers(fs, &fs->chunks[i], 0);
    /* subtract every allocated extent */
    bkey_t z = { 0, 0, 0 };
    r = btrfs_seek(fs, ext, &z, &p);
    while (r == 0) {
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        uint64_t len = 0;
        if (ik.type == BTRFS_EXTENT_ITEM_KEY) len = ik.offset;
        else if (ik.type == BTRFS_METADATA_ITEM_KEY) len = fs->nodesize;
        if (len) {
            bgroup_t *g = bg_find(fs, ik.objectid);
            if (g) range_remove(&g->free, ik.objectid, len);
        }
        r = btrfs_next_item(fs, &p);
    }
    path_release(&p);
    if (r < 0) return r;
    fs->space_loaded = true;
    return 0;
}

static int alloc_range(btrfs_t *fs, uint64_t kind, uint64_t len, uint64_t *out) {
    uint64_t *hint = kind == BG_DATA ? &fs->alloc_hint_data : &fs->alloc_hint_meta;
    for (int pass = 0; pass < 2; pass++) {
        for (bgroup_t *g = fs->bgroups; g; g = g->next) {
            if (!(g->flags & kind)) continue;
            if (kind != BG_SYSTEM && (g->flags & BG_SYSTEM)) continue;
            for (frange_t *r = g->free; r; r = r->next) {
                uint64_t start = r->start;
                if (pass == 0 && start + r->len <= *hint) continue;
                if (pass == 0 && start < *hint) start = *hint;
                /* tree blocks must be aligned to the node size (and not cross a 64 KiB stripe) */
                if (kind != BG_DATA) start = ALIGN_UP(start, fs->nodesize);
                else start = ALIGN_UP(start, fs->sectorsize);
                if (start + len > r->start + r->len) continue;
                range_remove(&g->free, start, len);
                g->used_delta += len;
                fs->bytes_used_delta += len;
                *hint = start + len;
                *out = start;
                return 0;
            }
        }
        *hint = 0;
    }
    return -ENOSPC;
}

static void release_range(btrfs_t *fs, uint64_t start, uint64_t len) {
    bgroup_t *g = bg_find(fs, start);
    if (g) range_insert(&g->free, start, len);
}

static void account_free(btrfs_t *fs, uint64_t start, uint64_t len) {
    bgroup_t *g = bg_find(fs, start);
    if (g) g->used_delta -= len;
    fs->bytes_used_delta -= len;
}

/* ------------------------------------------------------------------ deferred ops */

static void queue_op(btrfs_t *fs, int type, uint64_t bytenr, uint64_t len, uint64_t root, uint64_t owner,
                     uint64_t offset) {
    xop_t *o = kzalloc(sizeof(xop_t));
    o->type = type;
    o->bytenr = bytenr;
    o->len = len;
    o->root = root;
    o->owner = owner;
    o->offset = offset;
    if (fs->ops_tail) fs->ops_tail->next = o; else fs->ops = o;
    fs->ops_tail = o;
}

/* ------------------------------------------------------------------ tree blocks */

static ebuf_t *alloc_block(btrfs_t *fs, uint64_t owner, int level) {
    uint64_t bytenr;
    uint64_t kind = owner == BTRFS_CHUNK_TREE_OBJECTID ? BG_SYSTEM : BG_METADATA;
    if (alloc_range(fs, kind, fs->nodesize, &bytenr) < 0) return 0;
    ebuf_t *stale = btrfs_eb_lookup(fs, bytenr);
    if (stale) {
        btrfs_eb_remove(fs, stale);
        stale->orphan = true;
        if (!stale->refs) { kfree(stale->data); kfree(stale); }
    }
    ebuf_t *e = kzalloc(sizeof(ebuf_t));
    e->data = kzalloc(fs->nodesize);
    e->bytenr = bytenr;
    e->dirty = true;
    e->refs = 1;
    memcpy(e->data + 0x20, fs->meta_uuid, 16);
    wr64le(e->data + H_BYTENR, bytenr);
    wr64le(e->data + H_FLAGS, MIXED_BACKREF_REV);
    memcpy(e->data + H_CHUNK_UUID, fs->chunk_uuid, 16);
    wr64le(e->data + H_GENERATION, fs->transid);
    wr64le(e->data + H_OWNER, owner);
    e->data[H_LEVEL] = (uint8_t)level;
    btrfs_eb_insert(fs, e);
    queue_op(fs, OP_ADD_TREE, bytenr, fs->nodesize, owner, level, 0);
    return e;
}

static void free_block(btrfs_t *fs, ebuf_t *e, uint64_t root) {
    uint64_t bytenr = e->bytenr;
    int level = blevel(e);
    bool fresh = false;
    if (rd64le(e->data + H_GENERATION) == fs->transid) {
        /* allocated in this transaction: simply forget it */
        xop_t *prev = 0;
        for (xop_t *o = fs->ops; o; prev = o, o = o->next) {
            if (o->type == OP_ADD_TREE && o->bytenr == bytenr) {
                if (prev) prev->next = o->next; else fs->ops = o->next;
                if (fs->ops_tail == o) fs->ops_tail = prev;
                kfree(o);
                fresh = true;
                break;
            }
        }
    }
    account_free(fs, bytenr, fs->nodesize);
    if (fresh) release_range(fs, bytenr, fs->nodesize);
    else {
        queue_op(fs, OP_DROP_TREE, bytenr, fs->nodesize, root, level, 0);
        range_insert(&fs->pinned, bytenr, fs->nodesize);
    }
    btrfs_eb_remove(fs, e);
    e->orphan = true;
    e->dirty = false;
    if (!e->refs) { kfree(e->data); kfree(e); }
}

/* is a tree block referenced more than once (snapshots)? */
static bool block_is_shared(btrfs_t *fs, uint64_t bytenr, int level) {
    btree_t *ext = btrfs_get_tree(fs, BTRFS_EXTENT_TREE_OBJECTID);
    uint8_t buf[64];
    bkey_t k = { bytenr, BTRFS_METADATA_ITEM_KEY, (uint64_t)level };
    if (!(fs->incompat & INCOMPAT_SKINNY_METADATA)) { k.type = BTRFS_EXTENT_ITEM_KEY; k.offset = fs->nodesize; }
    int r = btrfs_lookup_item(fs, ext, &k, buf, sizeof(buf));
    if (r < 0 && (fs->incompat & INCOMPAT_SKINNY_METADATA)) {
        k.type = BTRFS_EXTENT_ITEM_KEY;
        k.offset = fs->nodesize;
        r = btrfs_lookup_item(fs, ext, &k, buf, sizeof(buf));
    }
    if (r < 24) return true;
    return rd64le(buf) != 1 || (rd64le(buf + 16) & BLOCK_FLAG_FULL_BACKREF);
}

static int cow_block(btrfs_t *fs, btree_t *t, path_t *p, int level) {
    ebuf_t *b = p->nodes[level];
    if (b->dirty && rd64le(b->data + H_GENERATION) == fs->transid) return 0;
    if (rd64le(b->data + H_OWNER) != t->id || block_is_shared(fs, b->bytenr, level)) {
        klog("[btrfs] block %lu of tree %lu is shared (snapshot?): refusing to modify\n", b->bytenr, t->id);
        return -EROFS;
    }
    ebuf_t *n = alloc_block(fs, t->id, level);
    if (!n) return -ENOSPC;
    memcpy(n->data + H_NRITEMS, b->data + H_NRITEMS, 4);
    memcpy(n->data + HDR_SIZE, b->data + HDR_SIZE, fs->nodesize - HDR_SIZE);
    if (level + 1 < BTRFS_MAX_LEVEL && p->nodes[level + 1]) {
        ebuf_t *parent = p->nodes[level + 1];
        int slot = p->slots[level + 1];
        wr64le(node_ptr(parent, slot) + 17, n->bytenr);
        wr64le(node_ptr(parent, slot) + 25, fs->transid);
    } else {
        t->bytenr = n->bytenr;
        t->level = (uint8_t)level;
        t->dirty = true;
    }
    free_block(fs, b, t->id);
    p->nodes[level] = n;
    btrfs_put_block(b);
    return 0;
}

static int insert_new_root(btrfs_t *fs, btree_t *t, path_t *p, int level) {
    ebuf_t *child = p->nodes[level - 1];
    ebuf_t *root = alloc_block(fs, t->id, level);
    if (!root) return -ENOSPC;
    bkey_t k = { 0, 0, 0 };
    if (nritems(child)) {
        if (blevel(child) == 0) item_key(child, 0, &k); else ptr_key(child, 0, &k);
    }
    set_ptr(root, 0, &k, child->bytenr, fs->transid);
    set_nritems(root, 1);
    t->bytenr = root->bytenr;
    t->level = (uint8_t)level;
    t->dirty = true;
    p->nodes[level] = root;
    p->slots[level] = 0;
    return 0;
}

static void insert_ptr(ebuf_t *parent, int slot, const bkey_t *k, uint64_t bytenr, uint64_t gen) {
    uint32_t n = nritems(parent);
    if ((uint32_t)slot < n) memmove(node_ptr(parent, slot + 1), node_ptr(parent, slot), (n - slot) * PTR_SIZE);
    set_ptr(parent, slot, k, bytenr, gen);
    set_nritems(parent, n + 1);
}

static void fixup_low_keys(path_t *p, const bkey_t *k, int level) {
    for (int l = level; l < BTRFS_MAX_LEVEL && p->nodes[l]; l++) {
        write_key(node_ptr(p->nodes[l], p->slots[l]), k);
        if (p->slots[l] != 0) break;
    }
}

static int split_node(btrfs_t *fs, btree_t *t, path_t *p, int level) {
    ebuf_t *c = p->nodes[level];
    if (!p->nodes[level + 1]) {
        int r = insert_new_root(fs, t, p, level + 1);
        if (r) return r;
    }
    ebuf_t *parent = p->nodes[level + 1];
    uint32_t n = nritems(c), mid = n / 2;
    ebuf_t *right = alloc_block(fs, t->id, level);
    if (!right) return -ENOSPC;
    memcpy(node_ptr(right, 0), node_ptr(c, mid), (n - mid) * PTR_SIZE);
    set_nritems(right, n - mid);
    set_nritems(c, mid);
    bkey_t k;
    ptr_key(right, 0, &k);
    insert_ptr(parent, p->slots[level + 1] + 1, &k, right->bytenr, fs->transid);
    btrfs_put_block(right);
    return 0;
}

static uint32_t items_bytes(ebuf_t *b, int from, int to) {
    uint32_t s = 0;
    for (int i = from; i < to; i++) s += ITEM_SIZE + item_size(b, i);
    return s;
}

static int split_leaf(btrfs_t *fs, btree_t *t, path_t *p, const bkey_t *key, int ins_len) {
    ebuf_t *l = p->nodes[0];
    int slot = p->slots[0];
    int n = (int)nritems(l);
    uint32_t lds = leaf_data_size(fs);
    if (!p->nodes[1]) {
        int r = insert_new_root(fs, t, p, 1);
        if (r) return r;
    }
    /* pick a split point where the new item fits */
    int mid = n / 2;
    bool to_right;
    if (slot < mid) to_right = false; else to_right = true;
    uint32_t side = to_right ? items_bytes(l, mid, n) : items_bytes(l, 0, mid);
    if (side + ins_len > lds) {
        mid = slot;
        if (items_bytes(l, 0, slot) + ins_len <= lds) to_right = false;
        else to_right = true;
    }
    ebuf_t *right = alloc_block(fs, t->id, 0);
    if (!right) return -ENOSPC;
    uint32_t dend = lds;
    for (int i = mid; i < n; i++) {
        bkey_t k;
        item_key(l, i, &k);
        uint32_t sz = item_size(l, i);
        dend -= sz;
        memcpy(ldata(right) + dend, item_data(l, i), sz);
        set_item(right, i - mid, &k, dend, sz);
    }
    set_nritems(right, n - mid);
    set_nritems(l, mid);
    bkey_t rk;
    if (n - mid > 0) item_key(right, 0, &rk); else rk = *key;
    insert_ptr(p->nodes[1], p->slots[1] + 1, &rk, right->bytenr, fs->transid);
    if (to_right) {
        btrfs_put_block(l);
        p->nodes[0] = right;
        p->slots[0] = slot - mid;
        p->slots[1]++;
    } else {
        btrfs_put_block(right);
    }
    if (leaf_free(fs, p->nodes[0]) < ins_len) return split_leaf(fs, t, p, key, ins_len);
    return 0;
}

/* search with optional copy-on-write of the whole path; ins_len > 0 guarantees that much
 * free space in the leaf (splitting as needed) */
int btrfs_search_cow(btrfs_t *fs, btree_t *t, const bkey_t *key, path_t *p, int ins_len, bool cow) {
    int restarts = 0;
again:
    path_release(p);
    if (++restarts > 16) return -EIO;
    ebuf_t *b = btrfs_read_block(fs, t->bytenr);
    if (!b) return -EIO;
    int level = blevel(b);
    p->nodes[level] = b;
    if (cow) {
        int r = cow_block(fs, t, p, level);
        if (r) { path_release(p); return r; }
        b = p->nodes[level];
    }
    for (;;) {
        bool found;
        int slot = btrfs_bin_search(b, key, &found);
        if (level == 0) {
            p->slots[0] = slot;
            if (ins_len > 0 && leaf_free(fs, b) < ins_len) {
                int r = split_leaf(fs, t, p, key, ins_len);
                if (r) { path_release(p); return r; }
            }
            return found ? 0 : 1;
        }
        if (!found && slot > 0) slot--;
        p->slots[level] = slot;
        if (cow && ins_len > 0 && nritems(b) >= max_ptrs(fs) - 1) {
            int r = split_node(fs, t, p, level);
            if (r) { path_release(p); return r; }
            goto again;
        }
        ebuf_t *c = btrfs_read_block(fs, ptr_block(b, slot));
        if (!c) { path_release(p); return -EIO; }
        level--;
        p->nodes[level] = c;
        if (cow) {
            int r = cow_block(fs, t, p, level);
            if (r) { path_release(p); return r; }
            c = p->nodes[level];
        }
        b = c;
    }
}

/* ------------------------------------------------------------------ item operations */

/* create an item of the given size (contents undefined); path points at it */
static int insert_empty(btrfs_t *fs, btree_t *t, const bkey_t *key, uint32_t size, path_t *p) {
    int r = btrfs_search_cow(fs, t, key, p, (int)(size + ITEM_SIZE), true);
    if (r == 0) return -EEXIST;
    if (r < 0) return r;
    ebuf_t *leaf = p->nodes[0];
    int slot = p->slots[0];
    uint32_t n = nritems(leaf);
    uint32_t data_end = leaf_data_end(fs, leaf), new_off;
    if ((uint32_t)slot < n) {
        uint32_t old_end = item_off(leaf, slot) + item_size(leaf, slot);
        memmove(ldata(leaf) + data_end - size, ldata(leaf) + data_end, old_end - data_end);
        for (uint32_t i = slot; i < n; i++) set_item_off(leaf, i, item_off(leaf, i) - size);
        memmove(leaf_item(leaf, slot + 1), leaf_item(leaf, slot), (n - slot) * ITEM_SIZE);
        new_off = old_end - size;
    } else {
        new_off = data_end - size;
    }
    set_item(leaf, slot, key, new_off, size);
    set_nritems(leaf, n + 1);
    if (slot == 0) fixup_low_keys(p, key, 1);
    return 0;
}

static int insert_item(btrfs_t *fs, btree_t *t, const bkey_t *key, const void *data, uint32_t size) {
    path_t p = { 0 };
    int r = insert_empty(fs, t, key, size, &p);
    if (r == 0 && size) memcpy(item_data(p.nodes[0], p.slots[0]), data, size);
    path_release(&p);
    return r;
}

static void del_ptr(btrfs_t *fs, btree_t *t, path_t *p, int level);

/* delete the item the path points at */
static void del_item(btrfs_t *fs, btree_t *t, path_t *p) {
    ebuf_t *leaf = p->nodes[0];
    int slot = p->slots[0];
    uint32_t n = nritems(leaf);
    uint32_t dsize = item_size(leaf, slot), doff = item_off(leaf, slot);
    uint32_t data_end = leaf_data_end(fs, leaf);
    if ((uint32_t)slot < n - 1) {
        memmove(ldata(leaf) + data_end + dsize, ldata(leaf) + data_end, doff - data_end);
        for (uint32_t i = slot + 1; i < n; i++) set_item_off(leaf, i, item_off(leaf, i) + dsize);
        memmove(leaf_item(leaf, slot), leaf_item(leaf, slot + 1), (n - slot - 1) * ITEM_SIZE);
    }
    set_nritems(leaf, n - 1);
    if (n - 1 == 0) {
        if (p->nodes[1]) {
            del_ptr(fs, t, p, 1);
            free_block(fs, leaf, t->id);
        }
    } else if (slot == 0) {
        bkey_t k;
        item_key(leaf, 0, &k);
        fixup_low_keys(p, &k, 1);
    }
}

static void del_ptr(btrfs_t *fs, btree_t *t, path_t *p, int level) {
    ebuf_t *node = p->nodes[level];
    int slot = p->slots[level];
    uint32_t n = nritems(node);
    if ((uint32_t)slot < n - 1) memmove(node_ptr(node, slot), node_ptr(node, slot + 1), (n - slot - 1) * PTR_SIZE);
    set_nritems(node, n - 1);
    if (n - 1 == 0) {
        if (level + 1 < BTRFS_MAX_LEVEL && p->nodes[level + 1]) {
            del_ptr(fs, t, p, level + 1);
            free_block(fs, node, t->id);
        } else {
            /* the root lost its last child: it becomes an empty leaf */
            node->data[H_LEVEL] = 0;
            t->level = 0;
            t->dirty = true;
        }
    } else if (slot == 0) {
        bkey_t k;
        ptr_key(node, 0, &k);
        fixup_low_keys(p, &k, level + 1);
    }
}

/* grow the item at the path by "extra" bytes at its end (space must be available) */
static void extend_item(btrfs_t *fs, path_t *p, uint32_t extra) {
    ebuf_t *leaf = p->nodes[0];
    int slot = p->slots[0];
    uint32_t n = nritems(leaf), data_end = leaf_data_end(fs, leaf);
    uint32_t old_end = item_off(leaf, slot) + item_size(leaf, slot);
    memmove(ldata(leaf) + data_end - extra, ldata(leaf) + data_end, old_end - data_end);
    for (uint32_t i = slot; i < n; i++) set_item_off(leaf, i, item_off(leaf, i) - extra);
    set_item_size(leaf, slot, item_size(leaf, slot) + extra);
}

/* shrink the item at the path to new_size (keeping its first bytes) */
static void truncate_item(btrfs_t *fs, path_t *p, uint32_t new_size) {
    ebuf_t *leaf = p->nodes[0];
    int slot = p->slots[0];
    uint32_t n = nritems(leaf), data_end = leaf_data_end(fs, leaf);
    uint32_t size = item_size(leaf, slot), diff = size - new_size;
    if (!diff) return;
    uint32_t keep_end = item_off(leaf, slot) + new_size;
    memmove(ldata(leaf) + data_end + diff, ldata(leaf) + data_end, keep_end - data_end);
    for (uint32_t i = slot; i < n; i++) set_item_off(leaf, i, item_off(leaf, i) + diff);
    set_item_size(leaf, slot, new_size);
}

/* COW search for an exact key; 0 = found (path valid), else error / -ENOENT */
static int find_cow(btrfs_t *fs, btree_t *t, const bkey_t *k, path_t *p, int ins_len) {
    int r = btrfs_search_cow(fs, t, k, p, ins_len, true);
    if (r == 1) { path_release(p); return -ENOENT; }
    return r;
}

static int delete_key(btrfs_t *fs, btree_t *t, const bkey_t *k) {
    path_t p = { 0 };
    int r = find_cow(fs, t, k, &p, -1);
    if (r == 0) del_item(fs, t, &p);
    path_release(&p);
    return r;
}

/* ------------------------------------------------------------------ free space tree */

static btree_t *fst_tree(btrfs_t *fs) {
    if (!(fs->compat_ro & COMPAT_RO_FREE_SPACE_TREE)) return 0;
    return btrfs_get_tree(fs, BTRFS_FREE_SPACE_TREE_OBJECTID);
}

static int fsi_update(btrfs_t *fs, btree_t *t, bgroup_t *g, int delta, uint32_t *flags_out) {
    bkey_t k = { g->start, BTRFS_FREE_SPACE_INFO_KEY, g->len };
    path_t p = { 0 };
    int r = find_cow(fs, t, &k, &p, 0);
    if (r) { path_release(&p); return r; }
    uint8_t *d = btrfs_path_data(&p, 0);
    if (flags_out) *flags_out = rd32le(d + 4);
    wr32le(d, (uint32_t)((int)rd32le(d) + delta));
    path_release(&p);
    return 0;
}

static int fsi_flags(btrfs_t *fs, btree_t *t, bgroup_t *g, uint32_t *flags) {
    bkey_t k = { g->start, BTRFS_FREE_SPACE_INFO_KEY, g->len };
    uint8_t d[8];
    int r = btrfs_lookup_item(fs, t, &k, d, 8);
    if (r < 0) return r;
    *flags = rd32le(d + 4);
    return 0;
}

/* bitmap helpers: find the bitmap item containing "pos" */
static int bitmap_find(btrfs_t *fs, btree_t *t, uint64_t pos, path_t *p, bool cow, uint64_t *bstart, uint64_t *blen) {
    bkey_t k = { pos, BTRFS_FREE_SPACE_BITMAP_KEY, (uint64_t)-1 };
    int r = cow ? btrfs_search_cow(fs, t, &k, p, 0, true) : btrfs_search(fs, t, &k, p);
    if (r < 0) return r;
    if (p->slots[0] == 0) return -ENOENT;
    p->slots[0]--;
    bkey_t ik;
    btrfs_path_key(p, &ik);
    if (ik.type != BTRFS_FREE_SPACE_BITMAP_KEY || pos < ik.objectid || pos >= ik.objectid + ik.offset) return -ENOENT;
    *bstart = ik.objectid;
    *blen = ik.offset;
    return 0;
}

static int bit_is_free(btrfs_t *fs, btree_t *t, bgroup_t *g, uint64_t pos) {
    if (pos < g->start || pos >= g->start + g->len) return 0;
    path_t p = { 0 };
    uint64_t bs, bl;
    int r = bitmap_find(fs, t, pos, &p, false, &bs, &bl);
    int v = 0;
    if (r == 0) {
        uint64_t bit = (pos - bs) / fs->sectorsize;
        v = (btrfs_path_data(&p, 0)[bit / 8] >> (bit % 8)) & 1;
    }
    path_release(&p);
    return v;
}

static int bitmap_set_range(btrfs_t *fs, btree_t *t, uint64_t start, uint64_t len, int val) {
    uint64_t pos = start, end = start + len;
    while (pos < end) {
        path_t p = { 0 };
        uint64_t bs, bl;
        int r = bitmap_find(fs, t, pos, &p, true, &bs, &bl);
        if (r) { path_release(&p); return r; }
        uint8_t *bits = btrfs_path_data(&p, 0);
        for (; pos < end && pos < bs + bl; pos += fs->sectorsize) {
            uint64_t bit = (pos - bs) / fs->sectorsize;
            if (val) bits[bit / 8] |= (uint8_t)(1u << (bit % 8));
            else bits[bit / 8] &= (uint8_t)~(1u << (bit % 8));
        }
        path_release(&p);
    }
    return 0;
}

/* the range [start, start+len) became used (alloc) or free */
static int fst_update(btrfs_t *fs, uint64_t start, uint64_t len, bool alloc) {
    btree_t *t = fst_tree(fs);
    if (!t) return 0;
    bgroup_t *g = bg_find(fs, start);
    if (!g) return -EIO;
    uint32_t flags;
    int r = fsi_flags(fs, t, g, &flags);
    if (r) return r;
    uint64_t end = start + len;
    if (flags & FSI_USING_BITMAPS) {
        int before = bit_is_free(fs, t, g, start - fs->sectorsize), after = bit_is_free(fs, t, g, end);
        r = bitmap_set_range(fs, t, start, len, alloc ? 0 : 1);
        if (r) return r;
        int delta;
        if (alloc) delta = (before && after) ? 1 : (!before && !after) ? -1 : 0;
        else delta = (before && after) ? -1 : (!before && !after) ? 1 : 0;
        return delta ? fsi_update(fs, t, g, delta, 0) : 0;
    }
    if (alloc) {
        /* remove the range from the free extents that contain it */
        int delta = 0;
        for (;;) {
            bkey_t k = { end, BTRFS_FREE_SPACE_EXTENT_KEY, 0 };
            path_t p = { 0 };
            r = btrfs_search(fs, t, &k, &p);
            if (r < 0) { path_release(&p); return r; }
            bool hit = false;
            bkey_t ik;
            if (p.slots[0] > 0) {
                p.slots[0]--;
                btrfs_path_key(&p, &ik);
                hit = ik.type == BTRFS_FREE_SPACE_EXTENT_KEY && ik.objectid < end && ik.objectid + ik.offset > start;
            }
            path_release(&p);
            if (!hit) break;
            uint64_t fs_ = ik.objectid, fe = ik.objectid + ik.offset;
            if ((r = delete_key(fs, t, &ik))) return r;
            delta--;
            if (fs_ < start) {
                bkey_t a = { fs_, BTRFS_FREE_SPACE_EXTENT_KEY, start - fs_ };
                if ((r = insert_item(fs, t, &a, 0, 0))) return r;
                delta++;
            }
            if (fe > end) {
                bkey_t b = { end, BTRFS_FREE_SPACE_EXTENT_KEY, fe - end };
                if ((r = insert_item(fs, t, &b, 0, 0))) return r;
                delta++;
            }
        }
        return delta ? fsi_update(fs, t, g, delta, 0) : 0;
    }
    /* free: merge with the neighbours */
    uint64_t ns = start, ne = end;
    int delta = 1;
    bkey_t k = { start, BTRFS_FREE_SPACE_EXTENT_KEY, 0 };
    path_t p = { 0 };
    r = btrfs_search(fs, t, &k, &p);
    if (r < 0) { path_release(&p); return r; }
    bkey_t prev = { 0, 0, 0 }, next = { 0, 0, 0 };
    bool have_prev = false, have_next = false;
    if (p.slots[0] > 0) {
        bkey_t ik;
        item_key(p.nodes[0], p.slots[0] - 1, &ik);
        if (ik.type == BTRFS_FREE_SPACE_EXTENT_KEY && ik.objectid + ik.offset == start && ik.objectid >= g->start) {
            prev = ik;
            have_prev = true;
        }
    }
    path_release(&p);
    bkey_t k2 = { end, BTRFS_FREE_SPACE_EXTENT_KEY, 0 };
    r = btrfs_seek(fs, t, &k2, &p);
    if (r == 0) {
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        if (ik.type == BTRFS_FREE_SPACE_EXTENT_KEY && ik.objectid == end && end < g->start + g->len) {
            next = ik;
            have_next = true;
        }
    }
    path_release(&p);
    if (r < 0) return r;
    if (have_prev) { if ((r = delete_key(fs, t, &prev))) return r; ns = prev.objectid; delta--; }
    if (have_next) { if ((r = delete_key(fs, t, &next))) return r; ne = next.objectid + next.offset; delta--; }
    bkey_t nk = { ns, BTRFS_FREE_SPACE_EXTENT_KEY, ne - ns };
    if ((r = insert_item(fs, t, &nk, 0, 0))) return r;
    return delta ? fsi_update(fs, t, g, delta, 0) : 0;
}

/* ------------------------------------------------------------------ checksums */

static int csum_insert(btrfs_t *fs, uint64_t bytenr, const uint8_t *data, uint64_t len) {
    btree_t *t = btrfs_get_tree(fs, BTRFS_CSUM_TREE_OBJECTID);
    if (!t) return -EIO;
    uint32_t per_item = (leaf_data_size(fs) / 2) / 4;
    uint64_t blocks = len / fs->sectorsize;
    for (uint64_t b = 0; b < blocks;) {
        uint32_t n = (uint32_t)MIN(blocks - b, (uint64_t)per_item);
        uint32_t *sums = kmalloc(n * 4);
        for (uint32_t i = 0; i < n; i++) sums[i] = btrfs_csum_data(data + (b + i) * fs->sectorsize, fs->sectorsize);
        bkey_t k = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, bytenr + b * fs->sectorsize };
        int r = insert_item(fs, t, &k, sums, n * 4);
        kfree(sums);
        if (r) return r;
        b += n;
    }
    return 0;
}

static int csum_delete(btrfs_t *fs, uint64_t start, uint64_t len) {
    btree_t *t = btrfs_get_tree(fs, BTRFS_CSUM_TREE_OBJECTID);
    if (!t) return 0;
    uint64_t end = start + len, ss = fs->sectorsize;
    for (int guard = 0; guard < 100000; guard++) {
        /* find a csum item overlapping [start, end): the last item starting before "end"
         * (searching for end - 1 keeps us in the leaf that holds it) */
        bkey_t k = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, end - 1 };
        path_t p = { 0 };
        int r = btrfs_search(fs, t, &k, &p);
        if (r < 0) { path_release(&p); return r; }
        if (p.slots[0] == 0) { path_release(&p); return 0; }
        p.slots[0]--;
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        uint32_t sz;
        uint8_t *d = btrfs_path_data(&p, &sz);
        uint64_t ks = ik.offset, ke = ks + (sz / 4) * ss;
        if (ik.objectid != BTRFS_EXTENT_CSUM_OBJECTID || ik.type != BTRFS_EXTENT_CSUM_KEY || ke <= start) {
            path_release(&p);
            return 0;
        }
        uint8_t *copy = kmalloc(sz);
        memcpy(copy, d, sz);
        path_release(&p);
        r = delete_key(fs, t, &ik);
        if (!r && ks < start) {
            bkey_t a = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, ks };
            r = insert_item(fs, t, &a, copy, (uint32_t)((start - ks) / ss * 4));
        }
        if (!r && ke > end) {
            bkey_t b = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, end };
            r = insert_item(fs, t, &b, copy + (end - ks) / ss * 4, (uint32_t)((ke - end) / ss * 4));
        }
        kfree(copy);
        if (r) return r;
    }
    return 0;
}

/* ------------------------------------------------------------------ extent tree ops */

static int apply_add_tree(btrfs_t *fs, xop_t *o) {
    btree_t *ext = btrfs_get_tree(fs, BTRFS_EXTENT_TREE_OBJECTID);
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    wr64le(buf, 1);
    wr64le(buf + 8, fs->transid);
    wr64le(buf + 16, EXTENT_FLAG_TREE_BLOCK);
    bkey_t k;
    uint32_t size;
    if (fs->incompat & INCOMPAT_SKINNY_METADATA) {
        k = (bkey_t){ o->bytenr, BTRFS_METADATA_ITEM_KEY, o->owner };
        buf[24] = BTRFS_TREE_BLOCK_REF_KEY;
        wr64le(buf + 25, o->root);
        size = 33;
    } else {
        k = (bkey_t){ o->bytenr, BTRFS_EXTENT_ITEM_KEY, fs->nodesize };
        /* tree_block_info: first key (left zero) + level */
        buf[24 + 17] = (uint8_t)o->owner;
        buf[42] = BTRFS_TREE_BLOCK_REF_KEY;
        wr64le(buf + 43, o->root);
        size = 51;
    }
    int r = insert_item(fs, ext, &k, buf, size);
    if (r) return r;
    return fst_update(fs, o->bytenr, o->len, true);
}

static int apply_drop_tree(btrfs_t *fs, xop_t *o) {
    btree_t *ext = btrfs_get_tree(fs, BTRFS_EXTENT_TREE_OBJECTID);
    bkey_t k = { o->bytenr, BTRFS_METADATA_ITEM_KEY, o->owner };
    if (!(fs->incompat & INCOMPAT_SKINNY_METADATA)) { k.type = BTRFS_EXTENT_ITEM_KEY; k.offset = fs->nodesize; }
    int r = delete_key(fs, ext, &k);
    if (r == -ENOENT && (fs->incompat & INCOMPAT_SKINNY_METADATA)) {
        k.type = BTRFS_EXTENT_ITEM_KEY;
        k.offset = fs->nodesize;
        r = delete_key(fs, ext, &k);
    }
    if (r) return r;
    return fst_update(fs, o->bytenr, o->len, false);
}

static int apply_add_data(btrfs_t *fs, xop_t *o) {
    btree_t *ext = btrfs_get_tree(fs, BTRFS_EXTENT_TREE_OBJECTID);
    uint8_t buf[53];
    wr64le(buf, 1);
    wr64le(buf + 8, fs->transid);
    wr64le(buf + 16, EXTENT_FLAG_DATA);
    buf[24] = BTRFS_EXTENT_DATA_REF_KEY;
    wr64le(buf + 25, o->root);
    wr64le(buf + 33, o->owner);
    wr64le(buf + 41, o->offset);
    wr32le(buf + 49, 1);
    bkey_t k = { o->bytenr, BTRFS_EXTENT_ITEM_KEY, o->len };
    int r = insert_item(fs, ext, &k, buf, sizeof(buf));
    if (r) return r;
    return fst_update(fs, o->bytenr, o->len, true);
}

/* change a data backref by +1/-1; drops the extent when its last reference goes */
static int apply_data_ref(btrfs_t *fs, xop_t *o, int delta) {
    btree_t *ext = btrfs_get_tree(fs, BTRFS_EXTENT_TREE_OBJECTID);
    bkey_t k = { o->bytenr, BTRFS_EXTENT_ITEM_KEY, o->len };
    path_t p = { 0 };
    int r = find_cow(fs, ext, &k, &p, delta > 0 ? 29 : 0);
    if (r) { path_release(&p); return r; }
    uint32_t sz;
    uint8_t *d = btrfs_path_data(&p, &sz);
    uint64_t refs = rd64le(d) + delta;
    wr64le(d, refs);
    /* inline references follow the 24-byte extent item */
    bool done = false;
    for (uint32_t off = 24; off < sz;) {
        uint8_t type = d[off];
        uint32_t rlen = type == BTRFS_EXTENT_DATA_REF_KEY ? 29 : type == BTRFS_SHARED_DATA_REF_KEY ? 13 : 9;
        if (type == BTRFS_EXTENT_DATA_REF_KEY && rd64le(d + off + 1) == o->root && rd64le(d + off + 9) == o->owner &&
            rd64le(d + off + 17) == o->offset) {
            uint32_t cnt = rd32le(d + off + 25) + delta;
            wr32le(d + off + 25, cnt);
            if (cnt == 0 && refs > 0) {
                memmove(d + off, d + off + rlen, sz - off - rlen);
                truncate_item(fs, &p, sz - rlen);
            }
            done = true;
            break;
        }
        off += rlen;
    }
    path_release(&p);
    if (!done) {
        /* keyed backref item */
        bkey_t rk = { o->bytenr, BTRFS_EXTENT_DATA_REF_KEY, btrfs_data_ref_hash(o->root, o->owner, o->offset) };
        path_t q = { 0 };
        r = btrfs_search_cow(fs, ext, &rk, &q, 0, true);
        if (r == 0) {
            uint8_t *dd = btrfs_path_data(&q, 0);
            uint32_t cnt = rd32le(dd + 24) + delta;
            wr32le(dd + 24, cnt);
            if (cnt == 0) del_item(fs, ext, &q);
            path_release(&q);
        } else {
            path_release(&q);
            if (r < 0) return r;
            if (delta < 0) return fail(fs, "data backref lookup", -EIO);
            uint8_t nb[28];
            wr64le(nb, o->root);
            wr64le(nb + 8, o->owner);
            wr64le(nb + 16, o->offset);
            wr32le(nb + 24, 1);
            if ((r = insert_item(fs, ext, &rk, nb, 28))) return r;
        }
    }
    if (refs == 0) {
        if ((r = delete_key(fs, ext, &k))) return r;
        account_free(fs, o->bytenr, o->len);
        range_insert(&fs->pinned, o->bytenr, o->len);
        if ((r = csum_delete(fs, o->bytenr, o->len))) return r;
        return fst_update(fs, o->bytenr, o->len, false);
    }
    return 0;
}

static int apply_ops(btrfs_t *fs) {
    while (fs->ops) {
        xop_t *o = fs->ops;
        fs->ops = o->next;
        if (!fs->ops) fs->ops_tail = 0;
        int r = 0;
        switch (o->type) {
        case OP_ADD_TREE: r = apply_add_tree(fs, o); break;
        case OP_DROP_TREE: r = apply_drop_tree(fs, o); break;
        case OP_ADD_DATA: r = apply_add_data(fs, o); break;
        case OP_ADD_DATA_REF: r = apply_data_ref(fs, o, 1); break;
        case OP_DROP_DATA_REF: r = apply_data_ref(fs, o, -1); break;
        }
        kfree(o);
        if (r) return fail(fs, "extent update", r);
    }
    return 0;
}

static int update_block_groups(btrfs_t *fs) {
    btree_t *t = bg_tree(fs);
    for (bgroup_t *g = fs->bgroups; g; g = g->next) {
        if (!g->used_delta) continue;
        int64_t delta = g->used_delta;
        g->used_delta = 0;
        bkey_t k = { g->start, BTRFS_BLOCK_GROUP_ITEM_KEY, g->len };
        path_t p = { 0 };
        int r = find_cow(fs, t, &k, &p, 0);
        if (r) { path_release(&p); return fail(fs, "block group update", r); }
        uint8_t *d = btrfs_path_data(&p, 0);
        wr64le(d, rd64le(d) + delta);
        g->used = rd64le(d);
        path_release(&p);
    }
    return 0;
}

static bool update_root_items(btrfs_t *fs) {
    bool changed = false;
    for (btree_t *t = fs->trees; t; t = t->next) {
        if (!t->dirty) continue;
        t->dirty = false;
        changed = true;
        path_t p = { 0 };
        int r = find_cow(fs, fs->root_tree, &t->root_key, &p, 0);
        if (r) { path_release(&p); fail(fs, "root item update", r); continue; }
        uint32_t sz;
        uint8_t *ri = btrfs_path_data(&p, &sz);
        wr64le(ri + RI_BYTENR, t->bytenr);
        ri[RI_LEVEL] = t->level;
        wr64le(ri + RI_GENERATION, fs->transid);
        if (sz > RI_GENERATION_V2 + 8) wr64le(ri + RI_GENERATION_V2, fs->transid);
        path_release(&p);
    }
    return changed;
}

/* ------------------------------------------------------------------ chunk allocation */

static uint64_t free_bytes(btrfs_t *fs, uint64_t kind) {
    uint64_t n = 0;
    for (bgroup_t *g = fs->bgroups; g; g = g->next) {
        if (!(g->flags & kind) || (kind != BG_SYSTEM && (g->flags & BG_SYSTEM))) continue;
        for (frange_t *r = g->free; r; r = r->next) n += r->len;
    }
    return n;
}

/* allocate a new chunk + block group of the given type on the (single) device */
static int alloc_chunk(btrfs_t *fs, uint64_t kind) {
    uint8_t *di = fs->super + SB_DEV_ITEM;
    uint64_t devid = rd64le(di), total = rd64le(di + 8);
    /* profile and stripe count follow the existing chunks of this type */
    uint64_t profile = 0;
    int nstripes = 1;
    for (int i = 0; i < fs->nchunks; i++)
        if ((fs->chunks[i].type & (BG_DATA | BG_METADATA | BG_SYSTEM)) == kind) {
            profile = fs->chunks[i].type & BG_DUP;
            nstripes = fs->chunks[i].num_stripes;
            break;
        }
    if (nstripes > 2) return -ENOSPC;
    uint64_t size = kind == BG_DATA ? (1ULL << 30) : (256ULL << 20);
    if (size > total / 10) size = (total / 10) & ~((1ULL << 20) - 1);
    if (size < (4ULL << 20)) size = 4ULL << 20;
    /* free physical space: gaps between device extents */
    btree_t *dev = btrfs_get_tree(fs, BTRFS_DEV_TREE_OBJECTID);
    if (!dev) return -EIO;
    uint64_t gaps[64][2];
    int ngaps = 0;
    uint64_t pos = 1ULL << 20;
    bkey_t k = { devid, BTRFS_DEV_EXTENT_KEY, 0 };
    path_t p = { 0 };
    int r = btrfs_seek(fs, dev, &k, &p);
    while (r == 0) {
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        if (ik.objectid != devid || ik.type != BTRFS_DEV_EXTENT_KEY) break;
        uint64_t len = rd64le(btrfs_path_data(&p, 0) + 24);
        if (ik.offset > pos && ngaps < 64) { gaps[ngaps][0] = pos; gaps[ngaps][1] = ik.offset - pos; ngaps++; }
        if (ik.offset + len > pos) pos = ik.offset + len;
        r = btrfs_next_item(fs, &p);
    }
    path_release(&p);
    if (r < 0) return r;
    if (total > pos && ngaps < 64) { gaps[ngaps][0] = pos; gaps[ngaps][1] = total - pos; ngaps++; }
    uint64_t phys[2] = { 0, 0 };
    for (; size >= (4ULL << 20); size /= 2) {
        int found = 0;
        for (int g = 0; g < ngaps && found < nstripes; g++) {
            uint64_t gs = ALIGN_UP(gaps[g][0], 1ULL << 20), ge = gaps[g][0] + gaps[g][1];
            while (found < nstripes && gs + size <= ge) { phys[found++] = gs; gs += size; }
        }
        if (found == nstripes) break;
    }
    if (size < (4ULL << 20)) return -ENOSPC;
    uint64_t logical = 0;
    for (int i = 0; i < fs->nchunks; i++)
        logical = MAX(logical, fs->chunks[i].logical + fs->chunks[i].length);
    logical = ALIGN_UP(logical, 1ULL << 20);
    uint64_t type = kind | profile;
    uint8_t dev_uuid[16];
    memcpy(dev_uuid, di + 66, 16);

    /* chunk item */
    uint32_t csz = 48 + 32 * nstripes;
    uint8_t ci[48 + 64];
    memset(ci, 0, sizeof(ci));
    wr64le(ci, size);
    wr64le(ci + 8, BTRFS_EXTENT_TREE_OBJECTID);
    wr64le(ci + 16, 65536);
    wr64le(ci + 24, type);
    wr32le(ci + 32, 65536);
    wr32le(ci + 36, 65536);
    wr32le(ci + 40, fs->sectorsize);
    wr16le(ci + 44, (uint16_t)nstripes);
    wr16le(ci + 46, 1);
    for (int s = 0; s < nstripes; s++) {
        wr64le(ci + 48 + s * 32, devid);
        wr64le(ci + 48 + s * 32 + 8, phys[s]);
        memcpy(ci + 48 + s * 32 + 16, dev_uuid, 16);
    }
    bkey_t ck = { BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_CHUNK_ITEM_KEY, logical };
    if ((r = insert_item(fs, fs->chunk_tree, &ck, ci, csz))) return fail(fs, "chunk item", r);
    /* device extents */
    for (int s = 0; s < nstripes; s++) {
        uint8_t de[48];
        wr64le(de, BTRFS_CHUNK_TREE_OBJECTID);
        wr64le(de + 8, BTRFS_FIRST_CHUNK_TREE_OBJECTID);
        wr64le(de + 16, logical);
        wr64le(de + 24, size);
        memcpy(de + 32, fs->chunk_uuid, 16);
        bkey_t dk = { devid, BTRFS_DEV_EXTENT_KEY, phys[s] };
        if ((r = insert_item(fs, dev, &dk, de, 48))) return fail(fs, "device extent", r);
    }
    /* device item usage */
    bkey_t dik = { BTRFS_DEV_ITEMS_OBJECTID, BTRFS_DEV_ITEM_KEY, devid };
    if ((r = find_cow(fs, fs->chunk_tree, &dik, &p, 0))) { path_release(&p); return fail(fs, "device item", r); }
    uint8_t *d = btrfs_path_data(&p, 0);
    wr64le(d + 16, rd64le(d + 16) + size * nstripes);
    wr64le(di + 16, rd64le(d + 16));
    path_release(&p);
    /* in-memory chunk + block group */
    uint8_t ci_disk[48 + 64];
    memcpy(ci_disk, ci, sizeof(ci_disk));
    extern int btrfs_chunk_add(btrfs_t *fs, uint64_t logical, const uint8_t *ci);
    btrfs_chunk_add(fs, logical, ci_disk);
    bgroup_t *g = kzalloc(sizeof(bgroup_t));
    g->start = logical;
    g->len = size;
    g->flags = type;
    range_insert(&g->free, logical, size);
    bgroup_t **tail = &fs->bgroups;
    while (*tail) tail = &(*tail)->next;
    *tail = g;
    uint64_t excl[6][2];
    int nex = MIN(exclude_supers(fs, &fs->chunks[fs->nchunks - 1], excl), 6);
    /* block group item */
    uint8_t bgi[24];
    wr64le(bgi, 0);
    wr64le(bgi + 8, BTRFS_FIRST_CHUNK_TREE_OBJECTID);
    wr64le(bgi + 16, type);
    bkey_t bk = { logical, BTRFS_BLOCK_GROUP_ITEM_KEY, size };
    if ((r = insert_item(fs, bg_tree(fs), &bk, bgi, 24))) return fail(fs, "block group item", r);
    /* free space tree: one extent per free range */
    btree_t *fst = fst_tree(fs);
    if (fst) {
        uint32_t count = 0;
        for (frange_t *fr = g->free; fr; fr = fr->next) {
            bkey_t fk = { fr->start, BTRFS_FREE_SPACE_EXTENT_KEY, fr->len };
            if ((r = insert_item(fs, fst, &fk, 0, 0))) return fail(fs, "free space extent", r);
            count++;
        }
        uint8_t info[8];
        wr32le(info, count);
        wr32le(info + 4, 0);
        bkey_t ik = { logical, BTRFS_FREE_SPACE_INFO_KEY, size };
        if ((r = insert_item(fs, fst, &ik, info, 8))) return fail(fs, "free space info", r);
    }
    (void)nex;
    klog("[btrfs] new %s chunk: %lu MiB at %lu\n", kind == BG_DATA ? "data" : "metadata", size >> 20, logical);
    return 0;
}

/* keep some metadata space available so commits never run out of room */
static void ensure_metadata_space(btrfs_t *fs) {
    if (fs->in_commit || fs->failed) return;
    uint64_t want = MAX(4ULL << 20, 64ULL * fs->nodesize);
    if (free_bytes(fs, BG_METADATA) < want) alloc_chunk(fs, BG_METADATA);
}

/* make sure "bytes" of data space are free beyond what buffered writes already hold */
static int check_data_space(btrfs_t *fs, uint64_t bytes) {
    if (free_bytes(fs, BG_DATA) >= fs->data_reserved + bytes) return 0;
    alloc_chunk(fs, BG_DATA);
    return free_bytes(fs, BG_DATA) >= fs->data_reserved + bytes ? 0 : -ENOSPC;
}

/* grow a node's reservation so its buffer of "buffered" bytes can be written */
static int reserve_buffer(btrfs_node_t *n, uint64_t buffered) {
    btrfs_t *fs = n->fs;
    uint64_t need = ALIGN_UP(buffered, fs->sectorsize) + 2 * fs->sectorsize;
    if (need <= n->wres) return 0;
    if (check_data_space(fs, need - n->wres) < 0) return -ENOSPC;
    fs->data_reserved += need - n->wres;
    n->wres = need;
    return 0;
}

static void release_buffer(btrfs_node_t *n) {
    btrfs_t *fs = n->fs;
    fs->data_reserved = fs->data_reserved > n->wres ? fs->data_reserved - n->wres : 0;
    n->wres = 0;
}

/* ------------------------------------------------------------------ transactions */

static int trans_start(btrfs_t *fs) {
    if (!fs->rw || fs->gone) return -EROFS;
    if (fs->failed) return -EIO;
    if (fs->in_trans) { ensure_metadata_space(fs); return 0; }
    int r = load_space(fs);
    if (r) return fail(fs, "loading free space", r);
    fs->transid = fs->generation + 1;
    fs->in_trans = true;
    ensure_metadata_space(fs);
    return 0;
}

int btrfs_commit(btrfs_t *fs) {
    for (btrfs_node_t *n = fs->nodes; n; n = n->next) btrfs_flush_node(n);
    if (!fs->in_trans || fs->failed) return fs->failed ? -EIO : 0;
    fs->in_commit = true;
    for (int iter = 0; iter < 64; iter++) {
        if (apply_ops(fs)) return -EIO;
        if (update_block_groups(fs)) return -EIO;
        if (fs->ops) continue;
        bool changed = update_root_items(fs);
        if (fs->failed) return -EIO;
        if (!fs->ops && !changed) break;
    }
    /* write every dirty tree block */
    for (int h = 0; h < 256; h++) {
        for (ebuf_t *e = fs->eb_hash[h]; e; e = e->hnext) {
            if (!e->dirty) continue;
            wr64le(e->data + H_FLAGS, rd64le(e->data + H_FLAGS) | HEADER_FLAG_WRITTEN);
            wr32le(e->data, btrfs_csum_data(e->data + 32, fs->nodesize - 32));
            memset(e->data + 4, 0, 28);
            if (btrfs_write_logical(fs, e->bytenr, e->data, fs->nodesize) < 0) return fail(fs, "writing tree blocks", -EIO);
            e->dirty = false;
        }
    }
    blk_flush(fs->dev);
    /* superblock (all mirrors that fit on the device) */
    uint8_t *sb = fs->super;
    wr64le(sb + SB_GENERATION, fs->transid);
    wr64le(sb + SB_ROOT, fs->root_tree->bytenr);
    sb[SB_ROOT_LEVEL] = fs->root_tree->level;
    wr64le(sb + SB_BYTES_USED, rd64le(sb + SB_BYTES_USED) + fs->bytes_used_delta);
    if (fs->chunk_tree->dirty) {
        wr64le(sb + SB_CHUNK_ROOT, fs->chunk_tree->bytenr);
        sb[SB_CHUNK_ROOT_LEVEL] = fs->chunk_tree->level;
        wr64le(sb + SB_CHUNK_ROOT_GEN, fs->transid);
        fs->chunk_tree->dirty = false;
    }
    fs->bytes_used_delta = 0;
    static const uint64_t mirrors[3] = { 0x10000ULL, 0x4000000ULL, 0x4000000000ULL };
    for (int m = 0; m < 3; m++) {
        if (mirrors[m] + 4096 > fs->dev->nsectors * 512) break;
        wr64le(sb + SB_BYTENR, mirrors[m]);
        wr32le(sb, btrfs_csum_data(sb + 32, 4096 - 32));
        if (blk_write(fs->dev, mirrors[m] / 512, 8, sb) < 0) return fail(fs, "writing the superblock", -EIO);
    }
    wr64le(sb + SB_BYTENR, mirrors[0]);
    blk_flush(fs->dev);
    fs->generation = fs->transid;
    fs->in_trans = false;
    fs->in_commit = false;
    /* space freed by this transaction can be reused now */
    for (frange_t *r = fs->pinned, *n; r; r = n) {
        n = r->next;
        release_range(fs, r->start, r->len);
        kfree(r);
    }
    fs->pinned = 0;
    return 0;
}

/* ------------------------------------------------------------------ inodes */

static int inode_store(btrfs_node_t *n) {
    btrfs_t *fs = n->fs;
    wr64le(n->inode + II_TRANSID, fs->transid);
    wr64le(n->inode + II_SEQUENCE, rd64le(n->inode + II_SEQUENCE) + 1);
    bkey_t k = { n->ino, BTRFS_INODE_ITEM_KEY, 0 };
    path_t p = { 0 };
    int r = find_cow(fs, n->root, &k, &p, 0);
    if (r == 0) memcpy(btrfs_path_data(&p, 0), n->inode, INODE_ITEM_SIZE);
    path_release(&p);
    n->vn->size = rd64le(n->inode + II_SIZE);
    n->vn->mtime = rd64le(n->inode + II_MTIME);
    return r;
}

static void set_time(uint8_t *ii, int field) {
    wr64le(ii + field, time_now());
    wr32le(ii + field + 8, 0);
}

static void touch(btrfs_node_t *n) {
    set_time(n->inode, II_MTIME);
    set_time(n->inode, II_CTIME);
}

/* ------------------------------------------------------------------ file extents */

typedef struct {
    bkey_t key;
    uint8_t fe[FE_SIZE];
    uint32_t size;
    uint64_t start, end;
} extent_ref_t;

static uint64_t fe_len(const uint8_t *fe, uint32_t size) {
    if (fe[FE_TYPE] == BTRFS_FILE_EXTENT_INLINE) return rd64le(fe + FE_RAM_BYTES);
    (void)size;
    return rd64le(fe + FE_NUM_BYTES);
}

/* find the first file extent item of the inode that ends after "s" and starts before "e" */
static int find_overlap(btrfs_node_t *n, uint64_t s, uint64_t e, extent_ref_t *out) {
    btrfs_t *fs = n->fs;
    bkey_t k = { n->ino, BTRFS_EXTENT_DATA_KEY, s };
    path_t p = { 0 };
    int r = btrfs_search(fs, n->root, &k, &p);
    if (r < 0) { path_release(&p); return r; }
    if (r == 1 && p.slots[0] > 0) p.slots[0]--;
    if (p.slots[0] >= (int)nritems(p.nodes[0]) && btrfs_next_item(fs, &p)) { path_release(&p); return 1; }
    for (;;) {
        bkey_t ik;
        btrfs_path_key(&p, &ik);
        if (ik.objectid > n->ino || (ik.objectid == n->ino && ik.type > BTRFS_EXTENT_DATA_KEY)) break;
        if (ik.objectid == n->ino && ik.type == BTRFS_EXTENT_DATA_KEY) {
            if (ik.offset >= e) break;
            uint32_t sz;
            uint8_t *fe = btrfs_path_data(&p, &sz);
            uint64_t len = fe_len(fe, sz);
            if (ik.offset + len > s) {
                out->key = ik;
                out->size = sz;
                memcpy(out->fe, fe, MIN(sz, (uint32_t)FE_SIZE));
                out->start = ik.offset;
                out->end = ik.offset + len;
                path_release(&p);
                return 0;
            }
        }
        if (btrfs_next_item(fs, &p)) break;
    }
    path_release(&p);
    return 1;
}

static bool fe_has_disk(const extent_ref_t *x) {
    return x->fe[FE_TYPE] != BTRFS_FILE_EXTENT_INLINE && rd64le(x->fe + FE_DISK_BYTENR) != 0;
}

/* nbytes contribution of [a, b) of an extent (holes count nothing) */
static uint64_t fe_bytes(const extent_ref_t *x, uint64_t a, uint64_t b) {
    if (x->fe[FE_TYPE] == BTRFS_FILE_EXTENT_INLINE) return rd64le(x->fe + FE_RAM_BYTES);
    if (!rd64le(x->fe + FE_DISK_BYTENR)) return 0;
    return b - a;
}

/* remove file extents from [s, e), splitting/trimming partial ones; returns bytes dropped */
static int64_t drop_extents(btrfs_node_t *n, uint64_t s, uint64_t e) {
    btrfs_t *fs = n->fs;
    int64_t dropped = 0;
    for (;;) {
        extent_ref_t x;
        int r = find_overlap(n, s, e, &x);
        if (r < 0) return r;
        if (r == 1) break;
        uint64_t ks = x.start, ke = x.end;
        uint64_t disk = rd64le(x.fe + FE_DISK_BYTENR), dnum = rd64le(x.fe + FE_DISK_NUM_BYTES);
        uint64_t eoff = rd64le(x.fe + FE_OFFSET);
        path_t p = { 0 };
        if ((r = find_cow(fs, n->root, &x.key, &p, 0))) { path_release(&p); return r; }
        if (ks >= s && ke <= e) {
            /* entirely inside */
            del_item(fs, n->root, &p);
            path_release(&p);
            dropped += fe_bytes(&x, ks, ke);
            if (fe_has_disk(&x)) queue_op(fs, OP_DROP_DATA_REF, disk, dnum, n->root->id, n->ino, ks - eoff);
        } else if (ks < s && ke > e) {
            /* split into [ks, s) and [e, ke) */
            uint8_t *fe = btrfs_path_data(&p, 0);
            wr64le(fe + FE_NUM_BYTES, s - ks);
            path_release(&p);
            uint8_t nfe[FE_SIZE];
            memcpy(nfe, x.fe, FE_SIZE);
            wr64le(nfe + FE_OFFSET, eoff + (e - ks));
            wr64le(nfe + FE_NUM_BYTES, ke - e);
            bkey_t nk = { n->ino, BTRFS_EXTENT_DATA_KEY, e };
            if ((r = insert_item(fs, n->root, &nk, nfe, FE_SIZE))) return r;
            dropped += fe_bytes(&x, s, e);
            if (fe_has_disk(&x)) queue_op(fs, OP_ADD_DATA_REF, disk, dnum, n->root->id, n->ino, ks - eoff);
            break;
        } else if (ks < s) {
            /* trim the end */
            uint8_t *fe = btrfs_path_data(&p, 0);
            wr64le(fe + FE_NUM_BYTES, s - ks);
            path_release(&p);
            dropped += fe_bytes(&x, s, ke);
        } else {
            /* trim the start: re-key the item at e */
            del_item(fs, n->root, &p);
            path_release(&p);
            uint8_t nfe[FE_SIZE];
            memcpy(nfe, x.fe, FE_SIZE);
            wr64le(nfe + FE_OFFSET, eoff + (e - ks));
            wr64le(nfe + FE_NUM_BYTES, ke - e);
            bkey_t nk = { n->ino, BTRFS_EXTENT_DATA_KEY, e };
            if ((r = insert_item(fs, n->root, &nk, nfe, FE_SIZE))) return r;
            dropped += fe_bytes(&x, ks, e);
            break;
        }
    }
    return dropped;
}

static int insert_hole(btrfs_node_t *n, uint64_t start, uint64_t len) {
    btrfs_t *fs = n->fs;
    if ((fs->incompat & INCOMPAT_NO_HOLES) || !len) return 0;
    uint8_t fe[FE_SIZE];
    memset(fe, 0, sizeof(fe));
    wr64le(fe + FE_GENERATION, fs->transid);
    wr64le(fe + FE_RAM_BYTES, len);
    fe[FE_TYPE] = BTRFS_FILE_EXTENT_REG;
    wr64le(fe + FE_NUM_BYTES, len);
    bkey_t k = { n->ino, BTRFS_EXTENT_DATA_KEY, start };
    return insert_item(fs, n->root, &k, fe, FE_SIZE);
}

/* write [off, off+len) of the file as one new extent (copy-on-write) */
static int write_extent_once(btrfs_node_t *n, uint64_t off, const uint8_t *data, uint64_t len) {
    btrfs_t *fs = n->fs;
    uint64_t ss = fs->sectorsize;
    uint64_t isize = rd64le(n->inode + II_SIZE);
    uint64_t s = off & ~(ss - 1), e = ALIGN_UP(off + len, ss);
    /* an inline extent is always replaced as a whole */
    extent_ref_t x;
    if (s > 0 && find_overlap(n, 0, 1, &x) == 0 && x.fe[FE_TYPE] == BTRFS_FILE_EXTENT_INLINE) {
        s = 0;
        if (e < ALIGN_UP(x.end, ss)) e = ALIGN_UP(x.end, ss);
    }
    uint64_t total = e - s;
    uint8_t *buf = kmalloc(total);
    if (!buf) return -ENOMEM;
    memset(buf, 0, total);
    if (s < off) btrfs_read_range(n, s, buf, off - s);
    if (off + len < e) btrfs_read_range(n, off + len, buf + (off + len - s), e - (off + len));
    memcpy(buf + (off - s), data, len);
    uint64_t bytenr;
    int r = alloc_range(fs, BG_DATA, total, &bytenr);
    if (r) { kfree(buf); return r; }
    r = btrfs_write_logical(fs, bytenr, buf, total);
    if (!r && !(rd64le(n->inode + II_FLAGS) & INODE_NODATASUM)) r = csum_insert(fs, bytenr, buf, total);
    kfree(buf);
    if (r) return fail(fs, "data write", r);
    uint64_t isize_al = ALIGN_UP(isize, ss);
    if (s > isize_al && (r = insert_hole(n, isize_al, s - isize_al))) return fail(fs, "hole", r);
    int64_t dropped = drop_extents(n, s, e);
    if (dropped < 0) return fail(fs, "dropping extents", (int)dropped);
    uint8_t fe[FE_SIZE];
    memset(fe, 0, sizeof(fe));
    wr64le(fe + FE_GENERATION, fs->transid);
    wr64le(fe + FE_RAM_BYTES, total);
    fe[FE_TYPE] = BTRFS_FILE_EXTENT_REG;
    wr64le(fe + FE_DISK_BYTENR, bytenr);
    wr64le(fe + FE_DISK_NUM_BYTES, total);
    wr64le(fe + FE_NUM_BYTES, total);
    bkey_t k = { n->ino, BTRFS_EXTENT_DATA_KEY, s };
    if ((r = insert_item(fs, n->root, &k, fe, FE_SIZE))) return fail(fs, "file extent", r);
    queue_op(fs, OP_ADD_DATA, bytenr, total, n->root->id, n->ino, s);
    wr64le(n->inode + II_NBYTES, rd64le(n->inode + II_NBYTES) + total - dropped);
    if (off + len > isize) wr64le(n->inode + II_SIZE, off + len);
    touch(n);
    return inode_store(n);
}

/* large writes are split when no contiguous free space is left */
static int write_extent(btrfs_node_t *n, uint64_t off, const uint8_t *data, uint64_t len) {
    uint64_t chunk = len;
    while (len) {
        uint64_t c = MIN(chunk, len);
        int r = write_extent_once(n, off, data, c);
        if (r == -ENOSPC && c > n->fs->sectorsize) { chunk = MAX(c / 2, (uint64_t)n->fs->sectorsize); continue; }
        if (r) return r;
        off += c;
        data += c;
        len -= c;
    }
    return 0;
}

void btrfs_flush_node(btrfs_node_t *n) {
    release_buffer(n);
    if (!n->wlen || n->dead) { n->wlen = 0; return; }
    uint32_t len = n->wlen;
    n->wlen = 0;
    if (trans_start(n->fs) == 0) write_extent(n, n->wstart, n->wbuf, len);
}

long btrfs_vn_write(vnode_t *vn, file_t *f, const void *buf, size_t len, uint64_t off) {
    UNUSED(f);
    btrfs_node_t *n = vn->priv;
    btrfs_t *fs = n->fs;
    int r = trans_start(fs);
    if (r) return r;
    if (vn->type == FT_DIR) return -EISDIR;
    if (!len) return 0;
    if (n->wlen && off == n->wstart + n->wlen && n->wlen + len <= WBUF_MAX) {
        if (reserve_buffer(n, n->wlen + len) < 0) return -ENOSPC;
        memcpy(n->wbuf + n->wlen, buf, len);
        n->wlen += (uint32_t)len;
    } else {
        btrfs_flush_node(n);
        if (fs->failed) return -EIO;
        if (len >= WBUF_MAX) {
            if (check_data_space(fs, ALIGN_UP(len, fs->sectorsize) + 2 * fs->sectorsize) < 0) return -ENOSPC;
            r = write_extent(n, off, buf, len);
            if (r) return r;
        } else {
            if (reserve_buffer(n, len) < 0) return -ENOSPC;
            if (!n->wbuf) n->wbuf = kmalloc(WBUF_MAX);
            if (!n->wbuf) return -ENOMEM;
            memcpy(n->wbuf, buf, len);
            n->wstart = off;
            n->wlen = (uint32_t)len;
        }
    }
    if (off + len > vn->size) vn->size = off + len;
    vn->mtime = time_now();
    return (long)len;
}

void btrfs_vn_close(vnode_t *vn, file_t *f) {
    UNUSED(f);
    btrfs_flush_node(vn->priv);
}

int btrfs_vn_truncate(vnode_t *vn, uint64_t size) {
    btrfs_node_t *n = vn->priv;
    btrfs_t *fs = n->fs;
    int r = trans_start(fs);
    if (r) return r;
    btrfs_flush_node(n);
    uint64_t old = rd64le(n->inode + II_SIZE), ss = fs->sectorsize;
    if (size < old) {
        int64_t dropped = drop_extents(n, ALIGN_UP(size, ss), (uint64_t)-1);
        if (dropped < 0) return fail(fs, "truncate", (int)dropped);
        wr64le(n->inode + II_NBYTES, rd64le(n->inode + II_NBYTES) - dropped);
        uint8_t *tail = 0;
        uint64_t bs = size & ~(ss - 1);
        if (size % ss) {
            tail = kmalloc(ss);
            btrfs_read_range(n, bs, tail, size - bs);
        }
        wr64le(n->inode + II_SIZE, size);
        if ((r = inode_store(n))) { kfree(tail); return fail(fs, "truncate", r); }
        if (tail) {
            /* rewrite the last block so no stale bytes remain after the new end */
            r = write_extent(n, bs, tail, size - bs);
            kfree(tail);
            if (r) return r;
        }
    } else if (size > old) {
        uint64_t oa = ALIGN_UP(old, ss), na = ALIGN_UP(size, ss);
        if (na > oa && (r = insert_hole(n, oa, na - oa))) return fail(fs, "hole", r);
        wr64le(n->inode + II_SIZE, size);
    }
    touch(n);
    return inode_store(n);
}

/* ------------------------------------------------------------------ directories */

static uint64_t next_index(btrfs_node_t *d) {
    if (!d->next_dir_index) {
        bkey_t k = { d->ino, BTRFS_DIR_INDEX_KEY, (uint64_t)-1 };
        path_t p = { 0 };
        uint64_t idx = 2;
        if (btrfs_search(d->fs, d->root, &k, &p) >= 0 && p.slots[0] > 0) {
            bkey_t ik;
            item_key(p.nodes[0], p.slots[0] - 1, &ik);
            if (ik.objectid == d->ino && ik.type == BTRFS_DIR_INDEX_KEY) idx = ik.offset + 1;
        }
        path_release(&p);
        d->next_dir_index = idx;
    }
    return d->next_dir_index++;
}

static uint64_t alloc_ino(btrfs_t *fs, btree_t *root) {
    btrfs_node_t *top = 0;
    for (btrfs_node_t *n = fs->nodes; n; n = n->next)
        if (n->root == root && n->ino == BTRFS_FIRST_FREE_OBJECTID) top = n;
    if (top && top->next_ino) return top->next_ino++;
    bkey_t k = { BTRFS_LAST_FREE_OBJECTID, 0, 0 };
    path_t p = { 0 };
    uint64_t ino = BTRFS_FIRST_FREE_OBJECTID + 1;
    if (btrfs_search(fs, root, &k, &p) >= 0 && p.slots[0] > 0) {
        bkey_t ik;
        item_key(p.nodes[0], p.slots[0] - 1, &ik);
        if (ik.objectid >= BTRFS_FIRST_FREE_OBJECTID && ik.objectid < BTRFS_LAST_FREE_OBJECTID) ino = ik.objectid + 1;
    }
    path_release(&p);
    if (top) top->next_ino = ino + 1;
    return ino;
}

static void make_dir_item(uint8_t *out, uint64_t ino, uint64_t transid, const char *name, size_t len, uint8_t type) {
    bkey_t loc = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    write_key(out, &loc);
    wr64le(out + DI_TRANSID, transid);
    wr16le(out + DI_DATA_LEN, 0);
    wr16le(out + DI_NAME_LEN, (uint16_t)len);
    out[DI_TYPE] = type;
    memcpy(out + DIR_ITEM_SIZE, name, len);
}

/* add "name" -> ino to directory d (DIR_ITEM, DIR_INDEX and the child's INODE_REF) */
static int link_entry(btrfs_node_t *d, uint64_t ino, const char *name, uint8_t type) {
    btrfs_t *fs = d->fs;
    size_t len = strlen(name);
    uint32_t isz = DIR_ITEM_SIZE + (uint32_t)len;
    uint8_t *di = kmalloc(isz);
    make_dir_item(di, ino, fs->transid, name, len, type);
    uint64_t index = next_index(d);
    /* DIR_ITEM (hash collisions share one item) */
    bkey_t k = { d->ino, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, len) };
    path_t p = { 0 };
    int r = btrfs_search_cow(fs, d->root, &k, &p, (int)isz, true);
    if (r == 0) {
        uint32_t old = item_size(p.nodes[0], p.slots[0]);
        extend_item(fs, &p, isz);
        memcpy(item_data(p.nodes[0], p.slots[0]) + old, di, isz);
        path_release(&p);
    } else {
        path_release(&p);
        if (r < 0 || (r = insert_item(fs, d->root, &k, di, isz))) { kfree(di); return r; }
    }
    bkey_t ik = { d->ino, BTRFS_DIR_INDEX_KEY, index };
    r = insert_item(fs, d->root, &ik, di, isz);
    kfree(di);
    if (r) return r;
    /* INODE_REF: index + name */
    uint8_t *ref = kmalloc(10 + len);
    wr64le(ref, index);
    wr16le(ref + 8, (uint16_t)len);
    memcpy(ref + 10, name, len);
    bkey_t rk = { ino, BTRFS_INODE_REF_KEY, d->ino };
    r = btrfs_search_cow(fs, d->root, &rk, &p, (int)(10 + len), true);
    if (r == 0) {
        uint32_t old = item_size(p.nodes[0], p.slots[0]);
        extend_item(fs, &p, 10 + (uint32_t)len);
        memcpy(item_data(p.nodes[0], p.slots[0]) + old, ref, 10 + len);
        path_release(&p);
    } else {
        path_release(&p);
        if (r > 0) r = insert_item(fs, d->root, &rk, ref, 10 + (uint32_t)len);
    }
    kfree(ref);
    if (r) return r;
    wr64le(d->inode + II_SIZE, rd64le(d->inode + II_SIZE) + 2 * len);
    touch(d);
    return inode_store(d);
}

/* remove "name" (pointing to ino) from directory d */
static int unlink_entry(btrfs_node_t *d, uint64_t ino, const char *name) {
    btrfs_t *fs = d->fs;
    size_t len = strlen(name);
    /* INODE_REF: find the index, remove the name */
    bkey_t rk = { ino, BTRFS_INODE_REF_KEY, d->ino };
    path_t p = { 0 };
    int r = find_cow(fs, d->root, &rk, &p, 0);
    if (r) { path_release(&p); return r; }
    uint32_t sz;
    uint8_t *ref = btrfs_path_data(&p, &sz);
    uint64_t index = 0;
    bool found = false;
    for (uint32_t off = 0; off + 10 <= sz;) {
        uint16_t nl = rd16le(ref + off + 8);
        if (nl == len && !memcmp(ref + off + 10, name, len)) {
            index = rd64le(ref + off);
            uint32_t el = 10 + nl;
            if (el == sz) del_item(fs, d->root, &p);
            else {
                memmove(ref + off, ref + off + el, sz - off - el);
                truncate_item(fs, &p, sz - el);
            }
            found = true;
            break;
        }
        off += 10 + nl;
    }
    path_release(&p);
    if (!found) return -ENOENT;
    /* DIR_INDEX */
    bkey_t ik = { d->ino, BTRFS_DIR_INDEX_KEY, index };
    if ((r = delete_key(fs, d->root, &ik)) && r != -ENOENT) return r;
    /* DIR_ITEM */
    bkey_t k = { d->ino, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, len) };
    if ((r = find_cow(fs, d->root, &k, &p, 0))) { path_release(&p); return r; }
    uint8_t *data = btrfs_path_data(&p, &sz);
    uint8_t *di = btrfs_dir_match(data, sz, name, len);
    if (di) {
        uint32_t el = DIR_ITEM_SIZE + rd16le(di + DI_NAME_LEN) + rd16le(di + DI_DATA_LEN);
        if (el == sz) del_item(fs, d->root, &p);
        else {
            uint32_t off = (uint32_t)(di - data);
            memmove(di, di + el, sz - off - el);
            truncate_item(fs, &p, sz - el);
        }
    }
    path_release(&p);
    wr64le(d->inode + II_SIZE, rd64le(d->inode + II_SIZE) - 2 * len);
    touch(d);
    return inode_store(d);
}

/* look up a name: its location key and entry type */
static int lookup_entry(btrfs_node_t *d, const char *name, bkey_t *loc, uint8_t *type) {
    size_t len = strlen(name);
    bkey_t k = { d->ino, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, len) };
    path_t p = { 0 };
    int r = btrfs_search(d->fs, d->root, &k, &p);
    if (r) { path_release(&p); return r < 0 ? r : -ENOENT; }
    uint32_t sz;
    uint8_t *data = btrfs_path_data(&p, &sz);
    uint8_t *di = btrfs_dir_match(data, sz, name, len);
    if (di) { read_key(di, loc); *type = di[DI_TYPE]; }
    path_release(&p);
    return di ? 0 : -ENOENT;
}

/* delete an inode whose last link went away */
static int destroy_inode(btrfs_node_t *c) {
    btrfs_t *fs = c->fs;
    c->wlen = 0;
    int64_t dropped = drop_extents(c, 0, (uint64_t)-1);
    if (dropped < 0) return (int)dropped;
    /* remove every remaining item of the inode (inode item, xattrs, ...) */
    for (;;) {
        bkey_t k = { c->ino, 0, 0 };
        path_t p = { 0 };
        int r = btrfs_seek(fs, c->root, &k, &p);
        bkey_t ik;
        if (r == 0) btrfs_path_key(&p, &ik);
        path_release(&p);
        if (r < 0) return r;
        if (r == 1 || ik.objectid != c->ino) break;
        if ((r = delete_key(fs, c->root, &ik))) return r;
    }
    c->dead = true;
    return 0;
}

int btrfs_vn_create(vnode_t *dir, const char *name, int type, vnode_t **out) {
    btrfs_node_t *d = dir->priv;
    btrfs_t *fs = d->fs;
    int r = trans_start(fs);
    if (r) return r;
    size_t len = strlen(name);
    if (!len || len > 255) return -ENAMETOOLONG;
    bkey_t loc;
    uint8_t t;
    if (lookup_entry(d, name, &loc, &t) == 0) return -EEXIST;
    uint64_t ino = alloc_ino(fs, d->root);
    uint8_t ii[INODE_ITEM_SIZE];
    memset(ii, 0, sizeof(ii));
    wr64le(ii + II_GENERATION, fs->transid);
    wr64le(ii + II_TRANSID, fs->transid);
    wr32le(ii + II_NLINK, 1);
    wr32le(ii + II_UID, rd32le(d->inode + II_UID));
    wr32le(ii + II_GID, rd32le(d->inode + II_GID));
    wr32le(ii + II_MODE, type == FT_DIR ? 040755 : 0100644);
    set_time(ii, II_ATIME);
    set_time(ii, II_CTIME);
    set_time(ii, II_MTIME);
    set_time(ii, II_OTIME);
    bkey_t k = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    if ((r = insert_item(fs, d->root, &k, ii, INODE_ITEM_SIZE))) return fail(fs, "create inode", r);
    if ((r = link_entry(d, ino, name, type == FT_DIR ? BTRFS_FT_DIR : BTRFS_FT_REG_FILE))) return fail(fs, "link", r);
    btrfs_node_t *n = btrfs_node_get(fs, d->root, ino);
    if (!n) return -EIO;
    n->vn->mnt = dir->mnt;
    *out = n->vn;
    return 0;
}

int btrfs_vn_unlink(vnode_t *dir, const char *name, bool dir_only) {
    btrfs_node_t *d = dir->priv;
    btrfs_t *fs = d->fs;
    int r = trans_start(fs);
    if (r) return r;
    bkey_t loc;
    uint8_t type;
    if ((r = lookup_entry(d, name, &loc, &type))) return r;
    if (loc.type != BTRFS_INODE_ITEM_KEY) return -EPERM;         /* subvolume */
    btrfs_node_t *c = btrfs_node_get(fs, d->root, loc.objectid);
    if (!c) return -EIO;
    bool is_dir = c->vn->type == FT_DIR;
    if (dir_only && !is_dir) { vnode_unref(c->vn); return -ENOTDIR; }
    if (!dir_only && is_dir) { vnode_unref(c->vn); return -EISDIR; }
    if (is_dir && rd64le(c->inode + II_SIZE) != 0) { vnode_unref(c->vn); return -ENOTEMPTY; }
    if ((r = unlink_entry(d, c->ino, name))) { vnode_unref(c->vn); return fail(fs, "unlink", r); }
    uint32_t nlink = rd32le(c->inode + II_NLINK);
    if (nlink <= 1 || is_dir) r = destroy_inode(c);
    else {
        wr32le(c->inode + II_NLINK, nlink - 1);
        set_time(c->inode, II_CTIME);
        r = inode_store(c);
    }
    vnode_unref(c->vn);
    return r ? fail(fs, "unlink", r) : 0;
}

int btrfs_vn_rename(vnode_t *odir, const char *oname, vnode_t *ndir, const char *nname) {
    btrfs_node_t *od = odir->priv, *nd = ndir->priv;
    btrfs_t *fs = od->fs;
    if (nd->fs != fs || nd->root != od->root) return -EXDEV;
    int r = trans_start(fs);
    if (r) return r;
    bkey_t loc, tloc;
    uint8_t type, ttype;
    if ((r = lookup_entry(od, oname, &loc, &type))) return r;
    if (loc.type != BTRFS_INODE_ITEM_KEY) return -EPERM;
    if (lookup_entry(nd, nname, &tloc, &ttype) == 0) {
        if (tloc.objectid == loc.objectid) return 0;
        r = btrfs_vn_unlink(ndir, nname, ttype == BTRFS_FT_DIR);
        if (r) return r;
    }
    if ((r = unlink_entry(od, loc.objectid, oname))) return fail(fs, "rename", r);
    if ((r = link_entry(nd, loc.objectid, nname, type))) return fail(fs, "rename", r);
    btrfs_node_t *c = btrfs_node_get(fs, od->root, loc.objectid);
    if (c) {
        set_time(c->inode, II_CTIME);
        inode_store(c);
        vnode_unref(c->vn);
    }
    return 0;
}
