/* rmdir - remove empty directories */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { fprintf(stderr, "usage: rmdir dir...\n"); return 1; }
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0) { fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
    return rc;
}
