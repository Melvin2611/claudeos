#pragma once
#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_REG 1
#define DT_DIR 2
#define DT_CHR 3
#define DT_FIFO 4

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    off_t d_size;
    time_t d_mtime;
    char d_name[256];
};

typedef struct DIR {
    int fd;
    struct dirent ent;
} DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
void rewinddir(DIR *d);
