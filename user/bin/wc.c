/* wc - count lines, words and bytes */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

static void count(FILE *f, long *l, long *w, long *c) {
    int ch, inword = 0;
    while ((ch = fgetc(f)) != EOF) {
        (*c)++;
        if (ch == '\n') (*l)++;
        if (isspace(ch)) inword = 0;
        else if (!inword) { inword = 1; (*w)++; }
    }
}

int main(int argc, char **argv) {
    long tl = 0, tw = 0, tc = 0;
    if (argc < 2) {
        count(stdin, &tl, &tw, &tc);
        printf("%7ld %7ld %7ld\n", tl, tw, tc);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno)); continue; }
        long l = 0, w = 0, c = 0;
        count(f, &l, &w, &c);
        fclose(f);
        printf("%7ld %7ld %7ld %s\n", l, w, c, argv[i]);
        tl += l; tw += w; tc += c;
    }
    if (argc > 2) printf("%7ld %7ld %7ld total\n", tl, tw, tc);
    return 0;
}
