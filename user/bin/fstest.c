/* fstest - file system stress test: fstest <directory> [files] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static unsigned seed = 12345;
static unsigned rnd(void) { seed = seed * 1103515245 + 12345; return (seed >> 16) & 0x7FFF; }

static void fill(unsigned char *b, size_t n, unsigned tag) {
    for (size_t i = 0; i < n; i++) b[i] = (unsigned char)(tag * 31 + i * 7 + (i >> 9));
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int write_file(const char *p, const unsigned char *b, size_t n) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < n) {
        size_t c = n - off < 3000 ? n - off : 3000;     /* odd-sized writes */
        if (write(fd, b + off, c) != (long)c) { close(fd); return -1; }
        off += c;
    }
    close(fd);
    return 0;
}

static int verify(const char *p, const unsigned char *b, size_t n) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char *r = malloc(n + 16);
    long got = 0, k;
    while ((k = read(fd, r + got, n + 16 - got)) > 0) got += k;
    close(fd);
    int ok = got == (long)n && !memcmp(r, b, n);
    free(r);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: fstest dir [files]\n"); return 1; }
    const char *dir = argv[1];
    int nfiles = argc > 2 ? atoi(argv[2]) : 200;
    char path[256], path2[256];
    snprintf(path, sizeof(path), "%s/fstest", dir);
    mkdir(path, 0755);
    size_t *sizes = calloc(nfiles, sizeof(size_t)), *orig = calloc(nfiles, sizeof(size_t));
    unsigned char *buf = malloc(300000), *buf2 = malloc(300000);

    printf("creating %d files...\n", nfiles);
    for (int i = 0; i < nfiles; i++) {
        sizes[i] = i % 10 == 0 ? 100000 + rnd() * 3 : rnd() % 9000;
        fill(buf, sizes[i], i);
        snprintf(path, sizeof(path), "%s/fstest/file_with_a_long_name_%04d.dat", dir, i);
        CHECK(write_file(path, buf, sizes[i]) == 0, "write %s", path);
    }
    printf("verifying...\n");
    for (int i = 0; i < nfiles; i++) {
        fill(buf, sizes[i], i);
        snprintf(path, sizeof(path), "%s/fstest/file_with_a_long_name_%04d.dat", dir, i);
        CHECK(verify(path, buf, sizes[i]) == 0, "verify %s", path);
    }
    printf("overwriting ranges, truncating...\n");
    for (int i = 0; i < nfiles; i += 7) {
        snprintf(path, sizeof(path), "%s/fstest/file_with_a_long_name_%04d.dat", dir, i);
        fill(buf, sizes[i], i);
        if (sizes[i] > 5000) {
            size_t off = sizes[i] / 3, len = sizes[i] / 4;
            for (size_t k = 0; k < len; k++) buf[off + k] ^= 0x5A;
            int fd = open(path, O_WRONLY);
            lseek(fd, off, SEEK_SET);
            write(fd, buf + off, len);
            close(fd);
            orig[i] = sizes[i];
            size_t nl = sizes[i] - 1234;
            fd = open(path, O_WRONLY);
            ftruncate(fd, nl);
            close(fd);
            sizes[i] = nl;
            CHECK(verify(path, buf, sizes[i]) == 0, "overwrite/truncate %s", path);
        }
    }
    printf("renaming and deleting...\n");
    for (int i = 0; i < nfiles; i++) {
        snprintf(path, sizeof(path), "%s/fstest/file_with_a_long_name_%04d.dat", dir, i);
        if (i % 3 == 0) CHECK(unlink(path) == 0, "unlink %s", path);
        else if (i % 3 == 1) {
            snprintf(path2, sizeof(path2), "%s/fstest/renamed_%04d", dir, i);
            CHECK(rename(path, path2) == 0, "rename %s", path);
        }
    }
    for (int i = 0; i < nfiles; i++) {
        if (i % 3 == 0) continue;
        if (i % 3 == 1) snprintf(path, sizeof(path), "%s/fstest/renamed_%04d", dir, i);
        else snprintf(path, sizeof(path), "%s/fstest/file_with_a_long_name_%04d.dat", dir, i);
        memset(buf2, 0, 16);
        fill(buf, sizes[i], i);
        if (orig[i]) {
            size_t off = orig[i] / 3, len = orig[i] / 4;
            for (size_t k = 0; k < len; k++) buf[off + k] ^= 0x5A;
        }
        CHECK(verify(path, buf, sizes[i]) == 0, "final verify %s", path);
    }
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
