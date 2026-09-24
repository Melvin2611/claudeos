/* touch - create empty files */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) { fprintf(stderr, "usage: touch file...\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) { fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        close(fd);
    }
    return rc;
}
