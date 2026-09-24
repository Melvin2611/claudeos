#pragma once
#include <sys/types.h>
#include <claudeos/abi.h>

struct stat {
    ino_t st_ino;
    mode_t st_mode;
    unsigned st_type;
    off_t st_size;
    time_t st_mtime;
    time_t st_ctime;
    dev_t st_dev;
};

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
