/* cp - copy files and directories (-r) */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static int opt_r;

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); return 1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno)); close(in); return 1; }
    char buf[8192];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) { fprintf(stderr, "cp: %s: write error\n", dst); rc = 1; break; }
    }
    close(in);
    close(out);
    return rc;
}

static int copy_path(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) < 0) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); return 1; }
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);
    if (!opt_r) { fprintf(stderr, "cp: %s is a directory (use -r)\n", src); return 1; }
    mkdir(dst, 0755);
    DIR *d = opendir(src);
    struct dirent *e;
    int rc = 0;
    while (d && (e = readdir(d))) {
        char s[512], t[512];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        rc |= copy_path(s, t);
    }
    if (d) closedir(d);
    return rc;
}

int main(int argc, char **argv) {
    int i = 1;
    if (i < argc && (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-R"))) { opt_r = 1; i++; }
    if (argc - i < 2) { fprintf(stderr, "usage: cp [-r] source... dest\n"); return 1; }
    const char *dest = argv[argc - 1];
    struct stat st;
    bool dest_dir = stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
    if (argc - i > 2 && !dest_dir) { fprintf(stderr, "cp: %s is not a directory\n", dest); return 1; }
    int rc = 0;
    for (; i < argc - 1; i++) {
        char target[512];
        if (dest_dir) {
            const char *base = strrchr(argv[i], '/');
            snprintf(target, sizeof(target), "%s/%s", dest, base ? base + 1 : argv[i]);
        } else {
            strlcpy(target, dest, sizeof(target));
        }
        rc |= copy_path(argv[i], target);
    }
    return rc;
}
