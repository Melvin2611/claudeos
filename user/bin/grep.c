/* grep - print lines matching a pattern (-i ignore case, -n line numbers, -v invert, -c count)
 * supports ^ and $ anchors, . and * like a minimal regular expression */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

static int opt_i, opt_n, opt_v, opt_c;

static int ceq(char a, char b) { return opt_i ? tolower((unsigned char)a) == tolower((unsigned char)b) : a == b; }

static int match_here(const char *re, const char *text);

static int match_star(char c, const char *re, const char *text) {
    do {
        if (match_here(re, text)) return 1;
    } while (*text && (c == '.' || ceq(*text, c)) && text++);
    return 0;
}

static int match_here(const char *re, const char *text) {
    if (!re[0]) return 1;
    if (re[1] == '*') return match_star(re[0], re + 2, text);
    if (re[0] == '$' && !re[1]) return *text == 0;
    if (*text && (re[0] == '.' || ceq(re[0], *text))) return match_here(re + 1, text + 1);
    return 0;
}

static int match(const char *re, const char *text) {
    if (re[0] == '^') return match_here(re + 1, text);
    do {
        if (match_here(re, text)) return 1;
    } while (*text++);
    return 0;
}

static int grep_file(const char *pat, FILE *f, const char *name, int multi) {
    char *line = 0;
    size_t cap = 0;
    long ln = 0, hits = 0;
    bool tty = isatty(1);
    while (getline(&line, &cap, f) > 0) {
        ln++;
        size_t l = strlen(line);
        if (l && line[l - 1] == '\n') line[--l] = 0;
        int m = match(pat, line);
        if (m == opt_v) continue;
        hits++;
        if (opt_c) continue;
        if (multi) printf(tty ? "\x1b[35m%s\x1b[0m:" : "%s:", name);
        if (opt_n) printf(tty ? "\x1b[32m%ld\x1b[0m:" : "%ld:", ln);
        printf("%s\n", line);
    }
    if (opt_c) {
        if (multi) printf("%s:", name);
        printf("%ld\n", hits);
    }
    free(line);
    return hits > 0;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (char *p = argv[i] + 1; *p; p++) {
            if (*p == 'i') opt_i = 1;
            else if (*p == 'n') opt_n = 1;
            else if (*p == 'v') opt_v = 1;
            else if (*p == 'c') opt_c = 1;
        }
    }
    if (i >= argc) { fprintf(stderr, "usage: grep [-invc] pattern [file...]\n"); return 2; }
    const char *pat = argv[i++];
    int found = 0;
    if (i >= argc) found = grep_file(pat, stdin, "(stdin)", 0);
    for (int k = i; k < argc; k++) {
        FILE *f = fopen(argv[k], "r");
        if (!f) { fprintf(stderr, "grep: %s: %s\n", argv[k], strerror(errno)); continue; }
        found |= grep_file(pat, f, argv[k], argc - i > 1);
        fclose(f);
    }
    return found ? 0 : 1;
}
