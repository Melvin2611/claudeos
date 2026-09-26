#pragma once
/* btrfs on-disk format (the subset we use) and driver internals */
#include <kernel.h>
#include <vfs.h>
#include <blk.h>

#define BTRFS_SUPER_OFFSET 0x10000ULL
#define BTRFS_MAGIC 0x4D5F53665248425FULL   /* "_BHRfS_M" */
#define BTRFS_MAX_LEVEL 8

/* superblock field offsets */
#define SB_FSID            0x20
#define SB_BYTENR          0x30
#define SB_FLAGS           0x38
#define SB_MAGIC           0x40
#define SB_GENERATION      0x48
#define SB_ROOT            0x50
#define SB_CHUNK_ROOT      0x58
#define SB_LOG_ROOT        0x60
#define SB_TOTAL_BYTES     0x70
#define SB_BYTES_USED      0x78
#define SB_NUM_DEVICES     0x88
#define SB_SECTORSIZE      0x90
#define SB_NODESIZE        0x94
#define SB_SYS_CHUNK_SIZE  0xA0
#define SB_INCOMPAT        0xBC
#define SB_COMPAT_RO       0xB4
#define SB_CSUM_TYPE       0xC4
#define SB_ROOT_LEVEL      0xC6
#define SB_CHUNK_ROOT_LEVEL 0xC7
#define SB_LOG_ROOT_LEVEL  0xC8
#define SB_LABEL           0x12B
#define SB_CACHE_GEN       0x22B
#define SB_METADATA_UUID   0x23B
#define SB_SYS_CHUNK_ARRAY 0x32B
#define SB_CHUNK_ROOT_GEN  0xA4
#define SB_DEV_ITEM        0xC9
#define BTRFS_DEV_ITEMS_OBJECTID 1ULL
#define BTRFS_DEV_EXTENT_KEY 204
#define BTRFS_DEV_ITEM_KEY 216

#define INCOMPAT_MIXED_BACKREF  (1ULL << 0)
#define INCOMPAT_DEFAULT_SUBVOL (1ULL << 1)
#define INCOMPAT_MIXED_GROUPS   (1ULL << 2)
#define INCOMPAT_COMPRESS_LZO   (1ULL << 3)
#define INCOMPAT_COMPRESS_ZSTD  (1ULL << 4)
#define INCOMPAT_BIG_METADATA   (1ULL << 5)
#define INCOMPAT_EXTENDED_IREF  (1ULL << 6)
#define INCOMPAT_RAID56         (1ULL << 7)
#define INCOMPAT_SKINNY_METADATA (1ULL << 8)
#define INCOMPAT_NO_HOLES       (1ULL << 9)
#define INCOMPAT_METADATA_UUID  (1ULL << 10)
#define COMPAT_RO_FREE_SPACE_TREE       (1ULL << 0)
#define COMPAT_RO_FREE_SPACE_TREE_VALID (1ULL << 1)
#define COMPAT_RO_VERITY                (1ULL << 2)
#define COMPAT_RO_BLOCK_GROUP_TREE      (1ULL << 3)

/* tree block header */
#define H_BYTENR     0x30
#define H_FLAGS      0x38
#define H_CHUNK_UUID 0x40
#define H_GENERATION 0x50
#define H_OWNER      0x58
#define H_NRITEMS    0x60
#define H_LEVEL      0x64
#define HDR_SIZE     0x65
#define ITEM_SIZE    25
#define PTR_SIZE     33
#define HEADER_FLAG_WRITTEN (1ULL << 0)
#define MIXED_BACKREF_REV   (1ULL << 56)

/* object ids */
#define BTRFS_ROOT_TREE_OBJECTID        1ULL
#define BTRFS_EXTENT_TREE_OBJECTID      2ULL
#define BTRFS_CHUNK_TREE_OBJECTID       3ULL
#define BTRFS_DEV_TREE_OBJECTID         4ULL
#define BTRFS_FS_TREE_OBJECTID          5ULL
#define BTRFS_CSUM_TREE_OBJECTID        7ULL
#define BTRFS_QUOTA_TREE_OBJECTID       8ULL
#define BTRFS_FREE_SPACE_TREE_OBJECTID  10ULL
#define BTRFS_BLOCK_GROUP_TREE_OBJECTID 11ULL
#define BTRFS_FIRST_FREE_OBJECTID       256ULL
#define BTRFS_LAST_FREE_OBJECTID        (-256ULL)
#define BTRFS_FIRST_CHUNK_TREE_OBJECTID 256ULL
#define BTRFS_EXTENT_CSUM_OBJECTID      (-10ULL)

/* item types */
#define BTRFS_INODE_ITEM_KEY      1
#define BTRFS_INODE_REF_KEY       12
#define BTRFS_INODE_EXTREF_KEY    13
#define BTRFS_XATTR_ITEM_KEY      24
#define BTRFS_DIR_ITEM_KEY        84
#define BTRFS_DIR_INDEX_KEY       96
#define BTRFS_EXTENT_DATA_KEY     108
#define BTRFS_EXTENT_CSUM_KEY     128
#define BTRFS_ROOT_ITEM_KEY       132
#define BTRFS_EXTENT_ITEM_KEY     168
#define BTRFS_METADATA_ITEM_KEY   169
#define BTRFS_TREE_BLOCK_REF_KEY  176
#define BTRFS_EXTENT_DATA_REF_KEY 178
#define BTRFS_SHARED_BLOCK_REF_KEY 182
#define BTRFS_SHARED_DATA_REF_KEY 184
#define BTRFS_BLOCK_GROUP_ITEM_KEY 192
#define BTRFS_FREE_SPACE_INFO_KEY 198
#define BTRFS_FREE_SPACE_EXTENT_KEY 199
#define BTRFS_FREE_SPACE_BITMAP_KEY 200
#define BTRFS_CHUNK_ITEM_KEY      228

/* block group / chunk types */
#define BG_DATA     (1ULL << 0)
#define BG_SYSTEM   (1ULL << 1)
#define BG_METADATA (1ULL << 2)
#define BG_RAID0    (1ULL << 3)
#define BG_RAID1    (1ULL << 4)
#define BG_DUP      (1ULL << 5)
#define BG_RAID10   (1ULL << 6)
#define BG_RAID5    (1ULL << 7)
#define BG_RAID6    (1ULL << 8)
#define BG_RAID1C3  (1ULL << 9)
#define BG_RAID1C4  (1ULL << 10)

/* extent item flags */
#define EXTENT_FLAG_DATA       1ULL
#define EXTENT_FLAG_TREE_BLOCK 2ULL
#define BLOCK_FLAG_FULL_BACKREF (1ULL << 8)

/* inode item */
#define INODE_ITEM_SIZE 160
#define II_GENERATION 0
#define II_TRANSID    8
#define II_SIZE       16
#define II_NBYTES     24
#define II_NLINK      40
#define II_UID        44
#define II_GID        48
#define II_MODE       52
#define II_FLAGS      64
#define II_SEQUENCE   72
#define II_ATIME      112
#define II_CTIME      124
#define II_MTIME      136
#define II_OTIME      148
#define INODE_NODATASUM (1ULL << 0)

/* root item */
#define RI_GENERATION  160
#define RI_ROOT_DIRID  168
#define RI_BYTENR      176
#define RI_BYTES_USED  192
#define RI_REFS        216
#define RI_LEVEL       238
#define RI_GENERATION_V2 239

/* dir item */
#define DIR_ITEM_SIZE  30
#define DI_TRANSID     17
#define DI_DATA_LEN    25
#define DI_NAME_LEN    27
#define DI_TYPE        29
#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR      2

/* file extent item */
#define FE_GENERATION    0
#define FE_RAM_BYTES     8
#define FE_COMPRESSION   16
#define FE_TYPE          20
#define FE_INLINE_DATA   21
#define FE_DISK_BYTENR   21
#define FE_DISK_NUM_BYTES 29
#define FE_OFFSET        37
#define FE_NUM_BYTES     45
#define FE_SIZE          53
#define BTRFS_FILE_EXTENT_INLINE   0
#define BTRFS_FILE_EXTENT_REG      1
#define BTRFS_FILE_EXTENT_PREALLOC 2

static inline uint16_t rd16le(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32le(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64le(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void wr16le(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void wr32le(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr64le(void *p, uint64_t v) { memcpy(p, &v, 8); }

typedef struct { uint64_t objectid; uint8_t type; uint64_t offset; } bkey_t;
static inline void read_key(const uint8_t *p, bkey_t *k) {
    k->objectid = rd64le(p);
    k->type = p[8];
    k->offset = rd64le(p + 9);
}
static inline void write_key(uint8_t *p, const bkey_t *k) {
    wr64le(p, k->objectid);
    p[8] = k->type;
    wr64le(p + 9, k->offset);
}
int bkey_cmp(const bkey_t *a, const bkey_t *b);

typedef struct ebuf {
    uint64_t bytenr;
    uint8_t *data;
    bool dirty;
    int refs;
    uint64_t used;
    bool orphan;                /* freed while still referenced: release on last put */
    struct ebuf *hnext;
} ebuf_t;

typedef struct {
    ebuf_t *nodes[BTRFS_MAX_LEVEL];
    int slots[BTRFS_MAX_LEVEL];
} path_t;

typedef struct btree {
    uint64_t id;
    bkey_t root_key;
    uint64_t bytenr;
    uint8_t level;
    bool dirty;                 /* root moved in this transaction */
    uint8_t root_item[439];
    uint32_t root_item_size;
    struct btree *next;
} btree_t;

typedef struct {
    uint64_t logical, length, type;
    int num_stripes;
    uint64_t stripe_dev[4], stripe_off[4];
} chunk_t;

/* free space of one block group (in memory, rebuilt from the extent tree) */
typedef struct frange { uint64_t start, len; struct frange *next; } frange_t;
typedef struct bgroup {
    uint64_t start, len, flags, used;
    int64_t used_delta;
    frange_t *free;             /* sorted free ranges */
    struct bgroup *next;
} bgroup_t;

/* deferred extent tree work (applied at commit) */
enum { OP_ADD_TREE, OP_DROP_TREE, OP_ADD_DATA, OP_DROP_DATA_REF, OP_ADD_DATA_REF };
typedef struct xop {
    int type;
    uint64_t bytenr, len;
    uint64_t root, owner, offset;   /* tree blocks: owner = level */
    struct xop *next;
} xop_t;

typedef struct btrfs_node btrfs_node_t;

typedef struct btrfs {
    blkdev_t *dev;
    uint8_t *super;
    uint32_t nodesize, sectorsize;
    uint16_t csum_type;
    uint64_t incompat, compat_ro, generation;
    uint8_t fsid[16], meta_uuid[16], chunk_uuid[16];
    char label[32];
    chunk_t *chunks;
    int nchunks, cap_chunks;
    btree_t *root_tree, *chunk_tree, *trees;
    ebuf_t *eb_hash[256];
    int eb_count;
    uint64_t eb_clock;
    btrfs_node_t *nodes;
    mount_t *mnt;
    bool rw, gone;
    /* write state */
    bool in_trans;
    uint64_t transid;
    bgroup_t *bgroups;
    bool space_loaded;
    xop_t *ops, *ops_tail;
    frange_t *pinned;
    int64_t bytes_used_delta;
    uint64_t alloc_hint_meta, alloc_hint_data;
    uint64_t data_reserved;     /* bytes promised to buffered writes */
    bool in_commit;
    bool failed;                /* a fatal error: stop writing */
} btrfs_t;

struct btrfs_node {
    btrfs_t *fs;
    btree_t *root;
    uint64_t ino;
    uint8_t inode[INODE_ITEM_SIZE];
    vnode_t *vn;
    bool dead;
    uint64_t rd_index, rd_next;
    uint64_t next_dir_index;
    uint64_t next_ino;          /* subvolume root node: next free inode number */
    /* buffered writes */
    uint8_t *wbuf;
    uint64_t wstart;
    uint32_t wlen;
    uint64_t wres;              /* data space reserved for the buffer */
    struct btrfs_node *next;
};

/* btrfs.c */
uint32_t btrfs_crc32c(uint32_t crc, const void *data, size_t n);
int btrfs_read_logical(btrfs_t *fs, uint64_t logical, void *buf, size_t len);
int btrfs_write_logical(btrfs_t *fs, uint64_t logical, const void *buf, size_t len);
ebuf_t *btrfs_read_block(btrfs_t *fs, uint64_t bytenr);
void btrfs_put_block(ebuf_t *e);
void path_release(path_t *p);
int btrfs_search(btrfs_t *fs, btree_t *t, const bkey_t *key, path_t *p);
int btrfs_bin_search(ebuf_t *b, const bkey_t *k, bool *found);
int btrfs_seek(btrfs_t *fs, btree_t *t, const bkey_t *key, path_t *p);
int btrfs_next_item(btrfs_t *fs, path_t *p);
void btrfs_path_key(path_t *p, bkey_t *k);
uint8_t *btrfs_path_data(path_t *p, uint32_t *size);
int btrfs_lookup_item(btrfs_t *fs, btree_t *t, const bkey_t *key, void *buf, uint32_t len);
btree_t *btrfs_get_tree(btrfs_t *fs, uint64_t id);
uint8_t *btrfs_dir_match(uint8_t *data, uint32_t size, const char *name, size_t len);
long btrfs_read_range(btrfs_node_t *n, uint64_t off, uint8_t *buf, size_t len);
btrfs_node_t *btrfs_node_get(btrfs_t *fs, btree_t *root, uint64_t ino);
uint32_t btrfs_csum_data(const void *data, size_t n);
uint64_t btrfs_name_hash(const char *name, size_t len);
uint64_t btrfs_data_ref_hash(uint64_t root, uint64_t owner, uint64_t offset);
void btrfs_eb_insert(btrfs_t *fs, ebuf_t *e);
void btrfs_eb_remove(btrfs_t *fs, ebuf_t *e);
ebuf_t *btrfs_eb_lookup(btrfs_t *fs, uint64_t bytenr);

/* btrfs_write.c */
int btrfs_search_cow(btrfs_t *fs, btree_t *t, const bkey_t *key, path_t *p, int ins_len, bool cow);
bool btrfs_can_write(btrfs_t *fs);
int btrfs_commit(btrfs_t *fs);
void btrfs_flush_node(btrfs_node_t *n);
long btrfs_vn_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off);
int btrfs_vn_create(vnode_t *dir, const char *name, int type, vnode_t **out);
int btrfs_vn_unlink(vnode_t *dir, const char *name, bool dir_only);
int btrfs_vn_rename(vnode_t *odir, const char *oname, vnode_t *ndir, const char *nname);
int btrfs_vn_truncate(vnode_t *vn, uint64_t size);
void btrfs_vn_close(vnode_t *vn, file_t *f);

/* btrfs_comp.c */
int btrfs_decompress(int type, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, uint32_t sectorsize);

/* block accessors */
static inline uint32_t nritems(ebuf_t *b) { return rd32le(b->data + H_NRITEMS); }
static inline uint8_t blevel(ebuf_t *b) { return b->data[H_LEVEL]; }
static inline void set_nritems(ebuf_t *b, uint32_t n) { wr32le(b->data + H_NRITEMS, n); }
static inline uint8_t *leaf_item(ebuf_t *b, int i) { return b->data + HDR_SIZE + i * ITEM_SIZE; }
static inline void item_key(ebuf_t *b, int i, bkey_t *k) { read_key(leaf_item(b, i), k); }
static inline uint32_t item_off(ebuf_t *b, int i) { return rd32le(leaf_item(b, i) + 17); }
static inline uint32_t item_size(ebuf_t *b, int i) { return rd32le(leaf_item(b, i) + 21); }
static inline uint8_t *item_data(ebuf_t *b, int i) { return b->data + HDR_SIZE + item_off(b, i); }
static inline uint8_t *node_ptr(ebuf_t *b, int i) { return b->data + HDR_SIZE + i * PTR_SIZE; }
static inline uint64_t ptr_block(ebuf_t *b, int i) { return rd64le(node_ptr(b, i) + 17); }
static inline void ptr_key(ebuf_t *b, int i, bkey_t *k) { read_key(node_ptr(b, i), k); }
