/* cat - concatenate files */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int cat_fd(int fd) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(1, buf, n) < 0) return 1;
    }
    return n < 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return cat_fd(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-")) { rc |= cat_fd(0); continue; }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        rc |= cat_fd(fd);
        close(fd);
    }
    return rc;
}
