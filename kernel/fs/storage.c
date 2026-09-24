/* Storage setup: find a FAT32 disk for /home, first-boot copy of the default home files, mkfs */
#include <kernel.h>
#include <vfs.h>
#include <blk.h>
#include <syscall.h>
#include <mm.h>

static bool home_on_disk;

static int copy_tree(const char *src, const char *dst) {
    kstat_t st;
    int r = vfs_stat(src, &st);
    if (r < 0) return r;
    if (st.type == FT_DIR) {
        vfs_mkdir(dst);
        file_t *d;
        if (vfs_open(src, O_RDONLY | O_DIRECTORY, &d) < 0) return -EIO;
        kdirent_t *e = kmalloc(sizeof(kdirent_t));
        char *s = kmalloc(PATH_MAX_LEN), *t = kmalloc(PATH_MAX_LEN);
        while (file_readdir(d, e) > 0) {
            snprintf(s, PATH_MAX_LEN, "%s/%s", src, e->name);
            snprintf(t, PATH_MAX_LEN, "%s/%s", dst, e->name);
            copy_tree(s, t);
        }
        kfree(e);
        kfree(s);
        kfree(t);
        file_close(d);
        return 0;
    }
    file_t *in, *out;
    if ((r = vfs_open(src, O_RDONLY, &in)) < 0) return r;
    if ((r = vfs_open(dst, O_WRONLY | O_CREAT | O_TRUNC, &out)) < 0) { file_close(in); return r; }
    uint8_t *buf = kmalloc(65536);
    long n;
    while ((n = file_read(in, buf, 65536)) > 0) {
        if (file_write(out, buf, n) != n) { r = -ENOSPC; break; }
    }
    kfree(buf);
    file_close(in);
    file_close(out);
    return r;
}

static void populate_home(void) {
    kstat_t st;
    if (vfs_stat("/home/.claudeos", &st) == 0) return;
    klog("[storage] first start on this disk: copying default files to /home\n");
    copy_tree("/etc/skel", "/home");
    vfs_write_all("/home/.claudeos", "ClaudeOS home directory\n", 24);
    fs_sync_all();
}

static bool try_mount_home(void) {
    for (blkdev_t *d = blk_list(); d; d = d->next) {
        if (fat_mount(d, "/home") == 0) return true;
    }
    return false;
}

/* background writer: flushes cached metadata every two seconds */
static int syncd(void *arg) {
    UNUSED(arg);
    for (;;) {
        sleep_ms(2000);
        fs_sync_all();
    }
    return 0;
}

void storage_init(void) {
    kthread_create("syncd", syncd, 0);
    /* the initrd's /home becomes the skeleton for new home directories */
    vfs_rename("/home", "/etc/skel");
    vfs_mkdir("/home");
    if (try_mount_home()) {
        home_on_disk = true;
        populate_home();
        return;
    }
    /* no usable disk: keep /home in RAM */
    vfs_rmdir("/home");
    vfs_rename("/etc/skel", "/home");
    klog("[storage] no FAT32 disk found - /home is kept in memory (changes are lost at shutdown)\n");
}

bool storage_home_persistent(void) { return home_on_disk; }

/* mkfs(device or NULL, label): format a whole disk as FAT32 and mount it on /home */
SYSCALL_DEF(sys_mkfs) {
    SYSCALL_UNUSED_ARGS;
    char name[16] = "", label[12] = "CLAUDEOS";
    if (a1 && strncpy_from_user(name, (const char *)a1, sizeof(name)) < 0) return -EFAULT;
    if (a2 && strncpy_from_user(label, (const char *)a2, sizeof(label)) < 0) return -EFAULT;
    blkdev_t *d = 0;
    if (name[0]) d = blk_find(name);
    else
        for (blkdev_t *b = blk_list(); b; b = b->next)
            if (!b->parent) { d = b; break; }
    if (!d) return -ENODEV;
    if (home_on_disk) return -EBUSY;
    int r = fat_mkfs(d, label);
    if (r < 0) return r;
    /* mount the fresh file system on /home, keeping the in-memory files */
    vfs_rename("/home", "/tmp/.oldhome");
    vfs_mkdir("/home");
    if (fat_mount(d, "/home") < 0) {
        vfs_rmdir("/home");
        vfs_rename("/tmp/.oldhome", "/home");
        return -EIO;
    }
    home_on_disk = true;
    copy_tree("/tmp/.oldhome", "/home");
    vfs_write_all("/home/.claudeos", "ClaudeOS home directory\n", 24);
    fs_sync_all();
    return 0;
}

void storage_register_syscalls(void) { syscall_register(SYS_MKFS, sys_mkfs); }
