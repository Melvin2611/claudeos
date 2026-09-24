/* rm - remove files (-r recursive, -f force) */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static int opt_r, opt_f;

static int rm_path(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (!opt_f) fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return opt_f ? 0 : 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!opt_r) { fprintf(stderr, "rm: %s: is a directory (use -r)\n", path); return 1; }
        DIR *d = opendir(path);
        struct dirent *e;
        int rc = 0;
        char names[64][256];
        /* collect first: removing while iterating would skip entries */
        for (;;) {
            int n = 0;
            rewinddir(d);
            while (n < 64 && (e = readdir(d))) strlcpy(names[n++], e->d_name, 256);
            if (!n) break;
            for (int i = 0; i < n; i++) {
                char full[512];
                snprintf(full, sizeof(full), "%s/%s", path, names[i]);
                if (rm_path(full)) { rc = 1; }
            }
            if (rc) break;
        }
        closedir(d);
        if (rmdir(path) < 0) { fprintf(stderr, "rm: %s: %s\n", path, strerror(errno)); return 1; }
        return rc;
    }
    if (unlink(path) < 0) { fprintf(stderr, "rm: %s: %s\n", path, strerror(errno)); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        for (char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r' || *p == 'R') opt_r = 1;
            else if (*p == 'f') opt_f = 1;
        }
    }
    if (i >= argc) { fprintf(stderr, "usage: rm [-rf] file...\n"); return 1; }
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "/") || !strcmp(argv[i], "/bin") || !strcmp(argv[i], "/system")) {
            fprintf(stderr, "rm: refusing to remove %s\n", argv[i]);
            rc = 1;
            continue;
        }
        rc |= rm_path(argv[i]);
    }
    return rc;
}
