/* initrd: unpack a USTAR archive into the VFS */
#include <kernel.h>
#include <vfs.h>

static uint64_t octal(const char *s, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && s[i]; i++) {
        if (s[i] < '0' || s[i] > '7') continue;
        v = v * 8 + (s[i] - '0');
    }
    return v;
}

static void mkdir_p(const char *path) {
    char buf[PATH_MAX_LEN];
    strlcpy(buf, path, sizeof(buf));
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            vfs_mkdir(buf);
            *p = '/';
        }
    }
    vfs_mkdir(buf);
}

void initrd_load(const void *data, size_t size, const char *dest) {
    const uint8_t *p = data;
    const uint8_t *end = p + size;
    int files = 0, dirs = 0;
    size_t bytes = 0;
    while (p + 512 <= end) {
        const char *h = (const char *)p;
        if (!h[0]) break;
        if (memcmp(h + 257, "ustar", 5)) { klog("[initrd] bad header, stopping\n"); break; }
        char name[256], full[PATH_MAX_LEN];
        char prefix[156];
        memcpy(prefix, h + 345, 155);
        prefix[155] = 0;
        char nm[101];
        memcpy(nm, h, 100);
        nm[100] = 0;
        if (prefix[0]) snprintf(name, sizeof(name), "%s/%s", prefix, nm);
        else strlcpy(name, nm, sizeof(name));
        const char *rel = name;
        while (rel[0] == '.' && rel[1] == '/') rel += 2;
        if (!strcmp(rel, ".") || !*rel) rel = "";
        snprintf(full, sizeof(full), "%s/%s", dest, rel);
        size_t len = strlen(full);
        while (len > 1 && full[len - 1] == '/') full[--len] = 0;
        uint64_t fsize = octal(h + 124, 12);
        char type = h[156];
        p += 512;
        if (type == '5') {
            if (*rel) { mkdir_p(full); dirs++; }
        } else if (type == '0' || type == 0) {
            file_t *f;
            int r = vfs_open(full, O_WRONLY | O_CREAT | O_TRUNC, &f);
            if (r == 0) {
                if (fsize) file_write(f, p, fsize);
                f->vn->mtime = octal(h + 136, 12);
                file_close(f);
                files++;
                bytes += fsize;
            } else {
                klog("[initrd] cannot create %s: %d\n", full, r);
            }
        }
        p += ALIGN_UP(fsize, 512);
    }
    klog("[initrd] unpacked %d files, %d directories (%lu KiB)\n", files, dirs, (uint64_t)(bytes >> 10));
}
