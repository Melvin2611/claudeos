/* ramfs: in-memory file system (root file system, populated from the initrd) */
#include <kernel.h>
#include <vfs.h>
#include <mm.h>

typedef struct rnode {
    char *name;
    vnode_t *vn;
    struct rnode *parent, *children, *next;
    uint8_t *data;
    size_t cap;
    bool unlinked;
} rnode_t;

static const vnode_ops_t ramfs_ops;

static rnode_t *rnode_new(const char *name, int type) {
    rnode_t *n = kzalloc(sizeof(rnode_t));
    n->name = strdup(name);
    n->vn = vnode_alloc(type, &ramfs_ops, n);
    return n;
}

static rnode_t *find_child(rnode_t *dir, const char *name) {
    for (rnode_t *c = dir->children; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return 0;
}

static void detach(rnode_t *n) {
    rnode_t *p = n->parent;
    if (!p) return;
    for (rnode_t **pp = &p->children; *pp; pp = &(*pp)->next) {
        if (*pp == n) { *pp = n->next; break; }
    }
    n->parent = 0;
    n->next = 0;
}

static void attach(rnode_t *dir, rnode_t *n) {
    n->parent = dir;
    n->next = dir->children;
    dir->children = n;
    dir->vn->mtime = time_now();
}

static int r_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    rnode_t *c = find_child(dir->priv, name);
    if (!c) return -ENOENT;
    vnode_ref(c->vn);
    *out = c->vn;
    return 0;
}

static int r_create(vnode_t *dir, const char *name, int type, vnode_t **out) {
    rnode_t *d = dir->priv;
    if (find_child(d, name)) return -EEXIST;
    rnode_t *n = rnode_new(name, type);
    n->vn->mnt = dir->mnt;
    attach(d, n);
    vnode_ref(n->vn);   /* one ref held by the tree, one returned */
    *out = n->vn;
    return 0;
}

static int r_unlink(vnode_t *dir, const char *name, bool dir_only) {
    rnode_t *d = dir->priv;
    rnode_t *c = find_child(d, name);
    if (!c) return -ENOENT;
    if (dir_only) {
        if (c->vn->type != FT_DIR) return -ENOTDIR;
        if (c->children) return -ENOTEMPTY;
    } else if (c->vn->type == FT_DIR) {
        return -EISDIR;
    }
    detach(c);
    d->vn->mtime = time_now();
    c->unlinked = true;
    vnode_unref(c->vn);   /* drop the tree's reference */
    return 0;
}

static int r_rename(vnode_t *odir, const char *oname, vnode_t *ndir, const char *nname) {
    rnode_t *src = find_child(odir->priv, oname);
    if (!src) return -ENOENT;
    rnode_t *dst = find_child(ndir->priv, nname);
    if (dst == src) return 0;
    if (dst) {
        if (dst->vn->type == FT_DIR) {
            if (src->vn->type != FT_DIR) return -EISDIR;
            if (dst->children) return -ENOTEMPTY;
        } else if (src->vn->type == FT_DIR) {
            return -ENOTDIR;
        }
        detach(dst);
        dst->unlinked = true;
        vnode_unref(dst->vn);
    }
    detach(src);
    kfree(src->name);
    src->name = strdup(nname);
    attach(ndir->priv, src);
    return 0;
}

static int r_readdir(vnode_t *dir, uint64_t index, kdirent_t *out) {
    rnode_t *d = dir->priv;
    uint64_t i = 0;
    for (rnode_t *c = d->children; c; c = c->next, i++) {
        if (i == index) {
            memset(out, 0, sizeof(*out));
            out->ino = c->vn->ino;
            out->type = c->vn->type;
            out->size = c->vn->size;
            out->mtime = c->vn->mtime;
            strlcpy(out->name, c->name, sizeof(out->name));
            return 1;
        }
    }
    return 0;
}

static long r_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    rnode_t *r = vn->priv;
    if (off >= vn->size) return 0;
    size_t avail = vn->size - off;
    if (n > avail) n = avail;
    memcpy(buf, r->data + off, n);
    return (long)n;
}

static int ensure_cap(rnode_t *r, size_t need) {
    if (need <= r->cap) return 0;
    size_t cap = r->cap ? r->cap : 256;
    while (cap < need) cap *= 2;
    uint8_t *d = krealloc(r->data, cap);
    if (!d) return -ENOSPC;
    r->data = d;
    r->cap = cap;
    return 0;
}

static long r_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) {
    rnode_t *r = vn->priv;
    if (off + n > (512ULL << 20)) return -EFBIG;
    if (ensure_cap(r, off + n) < 0) return -ENOSPC;
    if (off > vn->size) memset(r->data + vn->size, 0, off - vn->size);
    memcpy(r->data + off, buf, n);
    if (off + n > vn->size) vn->size = off + n;
    return (long)n;
}

static int r_truncate(vnode_t *vn, uint64_t size) {
    rnode_t *r = vn->priv;
    if (size > vn->size) {
        if (ensure_cap(r, size) < 0) return -ENOSPC;
        memset(r->data + vn->size, 0, size - vn->size);
    }
    vn->size = size;
    if (size == 0 && r->cap > 4096) {
        kfree(r->data);
        r->data = 0;
        r->cap = 0;
    }
    vn->mtime = time_now();
    return 0;
}

static void r_release(vnode_t *vn) {
    rnode_t *r = vn->priv;
    if (!r->unlinked) {
        /* still in the tree (should not happen): keep */
        vn->refcount = 1;
        return;
    }
    kfree(r->data);
    kfree(r->name);
    kfree(r);
    kfree(vn);
}

static const vnode_ops_t ramfs_ops = {
    .lookup = r_lookup, .create = r_create, .unlink = r_unlink, .rename = r_rename,
    .readdir = r_readdir, .read = r_read, .write = r_write, .truncate = r_truncate,
    .release = r_release,
};

static int ramfs_statfs(mount_t *m, kstatfs_t *st) {
    st->block_size = 4096;
    st->total_bytes = pmm_total_pages() * PAGE_SIZE;
    st->free_bytes = pmm_free_pages() * PAGE_SIZE;
    return 0;
}

const fs_ops_t ramfs_fs_ops = { .statfs = ramfs_statfs };

vnode_t *ramfs_create_root(void) {
    rnode_t *root = rnode_new("", FT_DIR);
    return root->vn;
}

void ramfs_register(void) {
    vnode_t *root = ramfs_create_root();
    vfs_mount("/", root, "ramfs", "ram", &ramfs_fs_ops, 0);
    root->mnt = vfs_mounts();
}
