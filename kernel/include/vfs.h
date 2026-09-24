#pragma once
#include <kernel.h>
#include <sched.h>
#include <claudeos/abi.h>

#define PATH_MAX_LEN 256
#define NAME_MAX_LEN 255

typedef struct vnode vnode_t;
typedef struct file file_t;
typedef struct mount mount_t;

typedef struct vnode_ops {
    int (*lookup)(vnode_t *dir, const char *name, vnode_t **out);
    int (*create)(vnode_t *dir, const char *name, int type, vnode_t **out);
    int (*unlink)(vnode_t *dir, const char *name, bool dir_only);
    int (*rename)(vnode_t *odir, const char *oname, vnode_t *ndir, const char *nname);
    int (*readdir)(vnode_t *dir, uint64_t index, kdirent_t *out);   /* 1 = entry, 0 = end, <0 err */
    long (*read)(vnode_t *vn, file_t *f, void *buf, size_t n, uint64_t off);
    long (*write)(vnode_t *vn, file_t *f, const void *buf, size_t n, uint64_t off);
    int (*truncate)(vnode_t *vn, uint64_t size);
    int (*ioctl)(vnode_t *vn, file_t *f, unsigned long req, void *arg);
    int (*poll)(vnode_t *vn, file_t *f);      /* returns POLLIN/POLLOUT/POLLHUP mask */
    int (*open)(vnode_t *vn, file_t *f);
    void (*close)(vnode_t *vn, file_t *f);
    void (*release)(vnode_t *vn);             /* last reference dropped */
    int (*sync)(vnode_t *vn);
} vnode_ops_t;

struct vnode {
    int type;                 /* FT_* */
    uint64_t size;
    uint64_t ino;
    uint64_t mtime, ctime;
    int refcount;
    const vnode_ops_t *ops;
    mount_t *mnt;
    mount_t *mounted_here;    /* filesystem mounted on this directory */
    void *priv;
};

struct file {
    vnode_t *vn;
    uint64_t off;
    int flags;
    int refcount;
    void *priv;
};

typedef struct fs_ops {
    int (*statfs)(mount_t *m, kstatfs_t *out);
    int (*sync)(mount_t *m);
} fs_ops_t;

struct mount {
    char path[64];
    char fstype[16];
    char device[32];
    vnode_t *root;
    vnode_t *covered;         /* directory this is mounted on */
    const fs_ops_t *ops;
    mutex_t lock;
    void *priv;
    mount_t *next;
};

void vfs_init(void);
int vfs_mount(const char *path, vnode_t *root, const char *fstype, const char *device, const fs_ops_t *ops, void *priv);
mount_t *vfs_mounts(void);

vnode_t *vnode_alloc(int type, const vnode_ops_t *ops, void *priv);
void vnode_ref(vnode_t *vn);
void vnode_unref(vnode_t *vn);

/* path operations (paths may be relative to the current task's cwd) */
int vfs_normalize(const char *cwd, const char *path, char *out);   /* absolute, no . or .. */
int vfs_lookup(const char *path, vnode_t **out);
int vfs_open(const char *path, int flags, file_t **out);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_rename(const char *from, const char *to);
int vfs_stat(const char *path, kstat_t *st);
int vfs_statfs(const char *path, kstatfs_t *st);

file_t *file_alloc(vnode_t *vn, int flags);
void file_ref(file_t *f);
void file_close(file_t *f);
long file_read(file_t *f, void *buf, size_t n);
long file_write(file_t *f, const void *buf, size_t n);
long file_seek(file_t *f, long off, int whence);
int file_readdir(file_t *f, kdirent_t *out);
int file_stat(file_t *f, kstat_t *st);
int file_truncate(file_t *f, uint64_t size);
int file_ioctl(file_t *f, unsigned long req, void *arg);
int file_poll(file_t *f);

/* convenience: read a whole file into a kmalloc'd buffer */
void *vfs_read_all(const char *path, size_t *size);
int vfs_write_all(const char *path, const void *data, size_t size);

/* fd table helpers (current task) */
int fd_install(task_t *t, file_t *f);          /* returns fd or -EMFILE */
file_t *fd_get(task_t *t, int fd);

/* ramfs */
vnode_t *ramfs_create_root(void);
void ramfs_register(void);
/* initrd */
void initrd_load(const void *data, size_t size, const char *dest);
/* devfs */
void devfs_init(void);
int devfs_register(const char *name, const vnode_ops_t *ops, void *priv);
/* pipes */
int pipe_create(file_t **rd, file_t **wr);
