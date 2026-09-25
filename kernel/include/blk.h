#pragma once
#include <kernel.h>

typedef struct blkdev {
    char name[16];
    char model[41];
    uint64_t nsectors;          /* 512-byte sectors */
    uint64_t offset;            /* partitions: first sector on the parent */
    struct blkdev *parent;      /* NULL for whole disks */
    int (*read)(struct blkdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blkdev *d);
    void *priv;
    bool mounted;
    struct blkdev *next;
} blkdev_t;

void blk_register(blkdev_t *d);
void blk_unregister(blkdev_t *d);
void storage_disk_removed(blkdev_t *disk);
blkdev_t *blk_list(void);
blkdev_t *blk_find(const char *name);
/* cached access to whole sectors of any block device (partitions are translated) */
int blk_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf);
int blk_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf);
int blk_flush(blkdev_t *d);
int blk_write_lazy(blkdev_t *d, uint64_t lba, const void *buf);   /* cached, written on flush */
void blk_scan_partitions(blkdev_t *disk);

void ata_init(void);
int fat_mount(blkdev_t *d, const char *path);
int fat_mkfs(blkdev_t *d, const char *label);
void fs_sync_all(void);
void storage_init(void);
int fs_mount_any(blkdev_t *d, const char *path, const char **fstype);
void storage_register_syscalls(void);
bool storage_home_persistent(void);
