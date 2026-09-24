/* hexdump - canonical hex + ASCII dump */
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    if (!f) { fprintf(stderr, "hexdump: %s: %s\n", argv[1], strerror(errno)); return 1; }
    unsigned char buf[16];
    size_t n, off = 0;
    while ((n = fread(buf, 1, 16, f)) > 0) {
        printf("%08zx  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", buf[i]); else printf("   ");
            if (i == 7) printf(" ");
        }
        printf(" |");
        for (size_t i = 0; i < n; i++) putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
        printf("|\n");
        off += n;
        if (n < 16) break;
    }
    printf("%08zx\n", off);
    if (f != stdin) fclose(f);
    return 0;
}
