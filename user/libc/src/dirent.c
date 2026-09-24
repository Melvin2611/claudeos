#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <claudeos.h>

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return 0;
    DIR *d = calloc(1, sizeof(DIR));
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d) {
    kdirent_t k;
    if (readdir_raw(d->fd, &k) <= 0) return 0;
    d->ent.d_ino = k.ino;
    d->ent.d_type = (unsigned char)k.type;
    d->ent.d_size = (off_t)k.size;
    d->ent.d_mtime = (time_t)k.mtime;
    strlcpy(d->ent.d_name, k.name, sizeof(d->ent.d_name));
    return &d->ent;
}

int closedir(DIR *d) {
    int r = close(d->fd);
    free(d);
    return r;
}

void rewinddir(DIR *d) { lseek(d->fd, 0, SEEK_SET); }
