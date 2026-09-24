/* mkdir - create directories (-p: parents) */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    int parents = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) { parents = 1; continue; }
        if (parents) {
            char buf[256];
            strlcpy(buf, argv[i], sizeof(buf));
            for (char *p = buf + 1; *p; p++) {
                if (*p == '/') { *p = 0; mkdir(buf, 0755); *p = '/'; }
            }
            if (mkdir(buf, 0755) < 0 && errno != EEXIST) { fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
        } else if (mkdir(argv[i], 0755) < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    if (argc < 2) { fprintf(stderr, "usage: mkdir [-p] dir...\n"); return 1; }
    return rc;
}
