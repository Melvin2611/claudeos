/* mv - move / rename files (falls back to copy+delete across file systems) */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: mv source... dest\n"); return 1; }
    const char *dest = argv[argc - 1];
    struct stat st;
    bool dest_dir = stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
    int rc = 0;
    for (int i = 1; i < argc - 1; i++) {
        char target[512];
        if (dest_dir) {
            const char *base = strrchr(argv[i], '/');
            snprintf(target, sizeof(target), "%s/%s", dest, base ? base + 1 : argv[i]);
        } else {
            strlcpy(target, dest, sizeof(target));
        }
        if (rename(argv[i], target) == 0) continue;
        if (errno == EXDEV) {
            char cmd[1100];
            snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s' && rm -r '%s'", argv[i], target, argv[i]);
            if (system(cmd) == 0) continue;
        }
        fprintf(stderr, "mv: %s: %s\n", argv[i], strerror(errno));
        rc = 1;
    }
    return rc;
}
