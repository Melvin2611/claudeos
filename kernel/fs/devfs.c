/* devfs: /dev with character devices registered by drivers */
#include <kernel.h>
#include <vfs.h>

typedef struct dev_entry {
    char name[32];
    vnode_t *vn;
    struct dev_entry *next;
} dev_entry_t;

static dev_entry_t *devices;
static vnode_t *devroot;

static int d_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    for (dev_entry_t *d = devices; d; d = d->next) {
        if (!strcmp(d->name, name)) {
            vnode_ref(d->vn);
            *out = d->vn;
            return 0;
        }
    }
    return -ENOENT;
}

static int d_readdir(vnode_t *dir, uint64_t index, kdirent_t *out) {
    uint64_t i = 0;
    for (dev_entry_t *d = devices; d; d = d->next, i++) {
        if (i == index) {
            memset(out, 0, sizeof(*out));
            out->ino = d->vn->ino;
            out->type = FT_CHR;
            strlcpy(out->name, d->name, sizeof(out->name));
            return 1;
        }
    }
    return 0;
}

static const vnode_ops_t devdir_ops = { .lookup = d_lookup, .readdir = d_readdir };

int devfs_register(const char *name, const vnode_ops_t *ops, void *priv) {
    dev_entry_t *d = kzalloc(sizeof(dev_entry_t));
    strlcpy(d->name, name, sizeof(d->name));
    d->vn = vnode_alloc(FT_CHR, ops, priv);
    d->vn->mnt = 0;   /* devices do their own locking */
    d->next = devices;
    devices = d;
    return 0;
}

/* ---- basic devices ---- */
static long null_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) { return 0; }
static long null_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) { return (long)n; }
static long zero_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) { memset(buf, 0, n); return (long)n; }

static uint64_t rng_state;
uint64_t krandom(void) {
    if (!rng_state) rng_state = rdtsc() | 1;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static long random_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    uint8_t *b = buf;
    rng_state ^= rdtsc();
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)krandom();
    return (long)n;
}

static long kmsg_read(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off) {
    return (long)klog_read(buf, off, n);
}
static long kmsg_write(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off) {
    char tmp[256];
    size_t m = MIN(n, sizeof(tmp) - 1);
    memcpy(tmp, buf, m);
    tmp[m] = 0;
    kprintf("%s", tmp);
    return (long)n;
}

static const vnode_ops_t null_ops = { .read = null_read, .write = null_write };
static const vnode_ops_t zero_ops = { .read = zero_read, .write = null_write };
static const vnode_ops_t random_ops = { .read = random_read, .write = null_write };
static const vnode_ops_t kmsg_ops = { .read = kmsg_read, .write = kmsg_write };

void devfs_init(void) {
    devroot = vnode_alloc(FT_DIR, &devdir_ops, 0);
    devfs_register("null", &null_ops, 0);
    devfs_register("zero", &zero_ops, 0);
    devfs_register("random", &random_ops, 0);
    devfs_register("urandom", &random_ops, 0);
    devfs_register("kmsg", &kmsg_ops, 0);
    vfs_mkdir("/dev");
    vfs_mount("/dev", devroot, "devfs", "dev", 0, 0);
    devroot->mnt = 0;
}
