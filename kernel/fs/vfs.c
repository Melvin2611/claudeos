/* Virtual file system: path resolution, mounts, open files, fd tables */
#include <kernel.h>
#include <vfs.h>

static vnode_t *root_vn;
static mount_t *mounts;
static uint64_t next_ino = 1;

vnode_t *vnode_alloc(int type, const vnode_ops_t *ops, void *priv) {
    vnode_t *vn = kzalloc(sizeof(vnode_t));
    vn->type = type;
    vn->ops = ops;
    vn->priv = priv;
    vn->refcount = 1;
    vn->ino = __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED);
    vn->mtime = vn->ctime = time_now();
    return vn;
}

void vnode_ref(vnode_t *vn) {
    if (vn) __atomic_add_fetch(&vn->refcount, 1, __ATOMIC_RELAXED);
}

void vnode_unref(vnode_t *vn) {
    if (!vn) return;
    if (__atomic_sub_fetch(&vn->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (vn->ops && vn->ops->release) vn->ops->release(vn);
        else kfree(vn);
    }
}

static void mnt_lock(vnode_t *vn) { if (vn && vn->mnt) mutex_lock(&vn->mnt->lock); }
static void mnt_unlock(vnode_t *vn) { if (vn && vn->mnt) mutex_unlock(&vn->mnt->lock); }

int vfs_mount(const char *path, vnode_t *root, const char *fstype, const char *device, const fs_ops_t *ops, void *priv) {
    mount_t *m = kzalloc(sizeof(mount_t));
    strlcpy(m->path, path, sizeof(m->path));
    strlcpy(m->fstype, fstype, sizeof(m->fstype));
    strlcpy(m->device, device, sizeof(m->device));
    m->root = root;
    m->ops = ops;
    m->priv = priv;
    root->mnt = m;
    if (!strcmp(path, "/")) {
        root_vn = root;
    } else {
        vnode_t *dir;
        int r = vfs_lookup(path, &dir);
        if (r < 0) { kfree(m); return r; }
        if (dir->type != FT_DIR) { vnode_unref(dir); kfree(m); return -ENOTDIR; }
        dir->mounted_here = m;
        m->covered = dir;   /* keep the reference */
    }
    /* append to list */
    mount_t **pp = &mounts;
    while (*pp) pp = &(*pp)->next;
    *pp = m;
    klog("[vfs] mounted %s (%s) on %s\n", device, fstype, path);
    return 0;
}

mount_t *vfs_mounts(void) { return mounts; }

/* detach a mount (lazily: files that are still open keep working or fail with I/O errors) */
int vfs_umount(const char *path) {
    for (mount_t **pp = &mounts; *pp; pp = &(*pp)->next) {
        mount_t *m = *pp;
        if (strcmp(m->path, path) || !m->covered) continue;
        if (m->ops && m->ops->sync) {
            mutex_lock(&m->lock);
            m->ops->sync(m);
            mutex_unlock(&m->lock);
        }
        *pp = m->next;
        m->covered->mounted_here = 0;
        vnode_unref(m->covered);
        m->covered = 0;
        if (m->ops && m->ops->umount) m->ops->umount(m);
        klog("[vfs] unmounted %s\n", path);
        return 0;
    }
    return -EINVAL;
}

/* flush every mounted file system */
void fs_sync_all(void) {
    for (mount_t *m = mounts; m; m = m->next) {
        if (!m->ops || !m->ops->sync) continue;
        mutex_lock(&m->lock);
        m->ops->sync(m);
        mutex_unlock(&m->lock);
    }
}

int vfs_normalize(const char *cwd, const char *path, char *out) {
    char tmp[PATH_MAX_LEN * 2];
    if (path[0] == '/') {
        if (strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp)) return -ENAMETOOLONG;
    } else {
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd && *cwd ? cwd : "/", path);
    }
    /* split into components */
    char *comps[64];
    int n = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') *p++ = 0;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (!strcmp(start, ".")) continue;
        if (!strcmp(start, "..")) { if (n > 0) n--; continue; }
        if (n >= 64) return -ENAMETOOLONG;
        if (strlen(start) > NAME_MAX_LEN) return -ENAMETOOLONG;
        comps[n++] = start;
    }
    size_t len = 0;
    out[0] = 0;
    if (n == 0) { strcpy(out, "/"); return 0; }
    for (int i = 0; i < n; i++) {
        size_t cl = strlen(comps[i]);
        if (len + cl + 2 > PATH_MAX_LEN) return -ENAMETOOLONG;
        out[len++] = '/';
        memcpy(out + len, comps[i], cl);
        len += cl;
        out[len] = 0;
    }
    return 0;
}

static vnode_t *cross_mount(vnode_t *vn) {
    while (vn && vn->mounted_here) {
        vnode_t *r = vn->mounted_here->root;
        vnode_ref(r);
        vnode_unref(vn);
        vn = r;
    }
    return vn;
}

/* walk an absolute normalized path; returns referenced vnode */
static int walk(const char *abspath, vnode_t **out) {
    vnode_t *vn = root_vn;
    if (!vn) return -ENOENT;
    vnode_ref(vn);
    vn = cross_mount(vn);
    const char *p = abspath;
    char name[NAME_MAX_LEN + 1];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t n = 0;
        while (*p && *p != '/') { if (n < NAME_MAX_LEN) name[n++] = *p; p++; }
        name[n] = 0;
        if (vn->type != FT_DIR || !vn->ops->lookup) { vnode_unref(vn); return -ENOTDIR; }
        vnode_t *next = 0;
        mnt_lock(vn);
        int r = vn->ops->lookup(vn, name, &next);
        mnt_unlock(vn);
        vnode_unref(vn);
        if (r < 0) return r;
        vn = cross_mount(next);
    }
    *out = vn;
    return 0;
}

static const char *task_cwd(void) { return current ? PROC(current)->cwd : "/"; }

int vfs_lookup(const char *path, vnode_t **out) {
    char abs[PATH_MAX_LEN];
    int r = vfs_normalize(task_cwd(), path, abs);
    if (r < 0) return r;
    return walk(abs, out);
}

/* resolve parent directory of path; name receives the last component */
static int resolve_parent(const char *path, vnode_t **dir, char *name) {
    char abs[PATH_MAX_LEN];
    int r = vfs_normalize(task_cwd(), path, abs);
    if (r < 0) return r;
    if (!strcmp(abs, "/")) return -EINVAL;
    char *slash = strrchr(abs, '/');
    strlcpy(name, slash + 1, NAME_MAX_LEN + 1);
    if (slash == abs) slash[1] = 0; else *slash = 0;
    r = walk(abs, dir);
    if (r < 0) return r;
    if ((*dir)->type != FT_DIR) { vnode_unref(*dir); return -ENOTDIR; }
    return 0;
}

file_t *file_alloc(vnode_t *vn, int flags) {
    file_t *f = kzalloc(sizeof(file_t));
    f->vn = vn;
    f->flags = flags;
    f->refcount = 1;
    return f;
}

void file_ref(file_t *f) { if (f) __atomic_add_fetch(&f->refcount, 1, __ATOMIC_RELAXED); }

void file_close(file_t *f) {
    if (!f) return;
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) > 0) return;
    if (f->vn && f->vn->ops->close) f->vn->ops->close(f->vn, f);
    vnode_unref(f->vn);
    kfree(f);
}

int vfs_open(const char *path, int flags, file_t **out) {
    vnode_t *vn = 0;
    int r = vfs_lookup(path, &vn);
    if (r == -ENOENT && (flags & O_CREAT)) {
        vnode_t *dir;
        char name[NAME_MAX_LEN + 1];
        r = resolve_parent(path, &dir, name);
        if (r < 0) return r;
        if (!dir->ops->create) { vnode_unref(dir); return -EROFS; }
        mnt_lock(dir);
        r = dir->ops->create(dir, name, FT_FILE, &vn);
        mnt_unlock(dir);
        vnode_unref(dir);
        if (r < 0) return r;
    } else if (r < 0) {
        return r;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vnode_unref(vn);
        return -EEXIST;
    }
    int acc = flags & O_ACCMODE;
    if (vn->type == FT_DIR && acc != O_RDONLY) { vnode_unref(vn); return -EISDIR; }
    if ((flags & O_DIRECTORY) && vn->type != FT_DIR) { vnode_unref(vn); return -ENOTDIR; }
    if ((flags & O_TRUNC) && vn->type == FT_FILE && acc != O_RDONLY) {
        if (!vn->ops->truncate) { vnode_unref(vn); return -EROFS; }
        mnt_lock(vn);
        r = vn->ops->truncate(vn, 0);
        mnt_unlock(vn);
        if (r < 0) { vnode_unref(vn); return r; }
    }
    file_t *f = file_alloc(vn, flags);
    if (vn->ops->open) {
        r = vn->ops->open(vn, f);
        if (r < 0) { f->vn = 0; kfree(f); vnode_unref(vn); return r; }
    }
    *out = f;
    return 0;
}

static bool uses_fs_lock(vnode_t *vn) { return vn->type == FT_FILE || vn->type == FT_DIR; }

long file_read(file_t *f, void *buf, size_t n) {
    vnode_t *vn = f->vn;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (vn->type == FT_DIR) return -EISDIR;
    if (!vn->ops->read) return -EINVAL;
    bool lk = uses_fs_lock(vn);
    if (lk) mnt_lock(vn);
    long r = vn->ops->read(vn, f, buf, n, f->off);
    if (r > 0) f->off += r;
    if (lk) mnt_unlock(vn);
    return r;
}

long file_write(file_t *f, const void *buf, size_t n) {
    vnode_t *vn = f->vn;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!vn->ops->write) return -EINVAL;
    bool lk = uses_fs_lock(vn);
    if (lk) mnt_lock(vn);
    if ((f->flags & O_APPEND) && vn->type == FT_FILE) f->off = vn->size;
    long r = vn->ops->write(vn, f, buf, n, f->off);
    if (r > 0) {
        f->off += r;
        if (vn->type == FT_FILE) vn->mtime = time_now();
    }
    if (lk) mnt_unlock(vn);
    return r;
}

long file_seek(file_t *f, long off, int whence) {
    if (f->vn->type == FT_PIPE || f->vn->type == FT_SOCK) return -ESPIPE;
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)f->off : (long)f->vn->size;
    if (whence < 0 || whence > 2) return -EINVAL;
    long pos = base + off;
    if (pos < 0) return -EINVAL;
    f->off = pos;
    return pos;
}

int file_readdir(file_t *f, kdirent_t *out) {
    vnode_t *vn = f->vn;
    if (vn->type != FT_DIR || !vn->ops->readdir) return -ENOTDIR;
    mnt_lock(vn);
    int r = vn->ops->readdir(vn, f->off, out);
    mnt_unlock(vn);
    if (r > 0) f->off++;
    return r;
}

static void fill_stat(vnode_t *vn, kstat_t *st) {
    memset(st, 0, sizeof(*st));
    st->ino = vn->ino;
    st->type = vn->type;
    st->size = vn->size;
    st->mtime = vn->mtime;
    st->ctime = vn->ctime;
    st->mode = vn->type == FT_DIR ? 0755 : 0644;
}

int file_stat(file_t *f, kstat_t *st) { fill_stat(f->vn, st); return 0; }

int file_truncate(file_t *f, uint64_t size) {
    vnode_t *vn = f->vn;
    if (vn->type != FT_FILE) return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!vn->ops->truncate) return -EROFS;
    mnt_lock(vn);
    int r = vn->ops->truncate(vn, size);
    mnt_unlock(vn);
    return r;
}

int file_ioctl(file_t *f, unsigned long req, void *arg) {
    if (f->vn->ops->ioctl) return f->vn->ops->ioctl(f->vn, f, req, arg);
    return -ENOTTY;
}

int file_poll(file_t *f) {
    if (f->vn->ops->poll) return f->vn->ops->poll(f->vn, f);
    return POLLIN | POLLOUT;
}

int vfs_stat(const char *path, kstat_t *st) {
    vnode_t *vn;
    int r = vfs_lookup(path, &vn);
    if (r < 0) return r;
    fill_stat(vn, st);
    vnode_unref(vn);
    return 0;
}

int vfs_statfs(const char *path, kstatfs_t *st) {
    vnode_t *vn;
    int r = vfs_lookup(path, &vn);
    if (r < 0) return r;
    mount_t *m = vn->mnt;
    memset(st, 0, sizeof(*st));
    if (m) {
        strlcpy(st->fstype, m->fstype, sizeof(st->fstype));
        if (m->ops && m->ops->statfs) m->ops->statfs(m, st);
    }
    vnode_unref(vn);
    return 0;
}

int vfs_mkdir(const char *path) {
    vnode_t *dir, *vn;
    char name[NAME_MAX_LEN + 1];
    int r = resolve_parent(path, &dir, name);
    if (r < 0) return r;
    if (!dir->ops->create) { vnode_unref(dir); return -EROFS; }
    mnt_lock(dir);
    vnode_t *existing;
    if (dir->ops->lookup(dir, name, &existing) == 0) {
        mnt_unlock(dir);
        vnode_unref(existing);
        vnode_unref(dir);
        return -EEXIST;
    }
    r = dir->ops->create(dir, name, FT_DIR, &vn);
    mnt_unlock(dir);
    vnode_unref(dir);
    if (r == 0) vnode_unref(vn);
    return r;
}

static int do_unlink(const char *path, bool dir_only) {
    vnode_t *dir;
    char name[NAME_MAX_LEN + 1];
    int r = resolve_parent(path, &dir, name);
    if (r < 0) return r;
    if (!dir->ops->unlink) { vnode_unref(dir); return -EROFS; }
    /* refuse to remove mount points */
    vnode_t *target;
    mnt_lock(dir);
    r = dir->ops->lookup(dir, name, &target);
    if (r == 0) {
        if (target->mounted_here) r = -EBUSY;
        vnode_unref(target);
    }
    if (r == 0) r = dir->ops->unlink(dir, name, dir_only);
    mnt_unlock(dir);
    vnode_unref(dir);
    return r;
}

int vfs_unlink(const char *path) { return do_unlink(path, false); }
int vfs_rmdir(const char *path) { return do_unlink(path, true); }

int vfs_rename(const char *from, const char *to) {
    vnode_t *d1, *d2;
    char n1[NAME_MAX_LEN + 1], n2[NAME_MAX_LEN + 1];
    int r = resolve_parent(from, &d1, n1);
    if (r < 0) return r;
    r = resolve_parent(to, &d2, n2);
    if (r < 0) { vnode_unref(d1); return r; }
    if (d1->mnt != d2->mnt) r = -EXDEV;
    else if (!d1->ops->rename) r = -EROFS;
    else {
        /* moving a directory into itself? */
        char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
        vfs_normalize(task_cwd(), from, a);
        vfs_normalize(task_cwd(), to, b);
        size_t al = strlen(a);
        if (!strncmp(a, b, al) && (b[al] == '/' )) r = -EINVAL;
        else {
            mnt_lock(d1);
            r = d1->ops->rename(d1, n1, d2, n2);
            mnt_unlock(d1);
        }
    }
    vnode_unref(d1);
    vnode_unref(d2);
    return r;
}

void *vfs_read_all(const char *path, size_t *size) {
    file_t *f;
    if (vfs_open(path, O_RDONLY, &f) < 0) return 0;
    size_t sz = f->vn->size;
    uint8_t *buf = kmalloc(sz + 1);
    if (!buf) { file_close(f); return 0; }
    size_t got = 0;
    while (got < sz) {
        long r = file_read(f, buf + got, sz - got);
        if (r <= 0) break;
        got += r;
    }
    buf[got] = 0;
    file_close(f);
    if (size) *size = got;
    return buf;
}

int vfs_write_all(const char *path, const void *data, size_t size) {
    file_t *f;
    int r = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if (r < 0) return r;
    long w = file_write(f, data, size);
    file_close(f);
    return w == (long)size ? 0 : (w < 0 ? (int)w : -EIO);
}

int fd_install(task_t *t, file_t *f) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!PROC(t)->fds[i]) { PROC(t)->fds[i] = f; return i; }
    }
    return -EMFILE;
}

file_t *fd_get(task_t *t, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return 0;
    return PROC(t)->fds[fd];
}

void vfs_init(void) {
    ramfs_register();
}
