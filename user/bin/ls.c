/* ls - list directory contents */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <claudeos.h>

static bool opt_long, opt_all, opt_one, opt_human;

typedef struct { char name[256]; struct stat st; } entry_t;

static int cmp(const void *a, const void *b) { return strcasecmp(((entry_t *)a)->name, ((entry_t *)b)->name); }

static const char *color_for(entry_t *e) {
    if (S_ISDIR(e->st.st_mode)) return "\x1b[1;34m";
    if (S_ISCHR(e->st.st_mode)) return "\x1b[1;33m";
    const char *dot = strrchr(e->name, '.');
    if (dot && (!strcmp(dot, ".bmp") || !strcmp(dot, ".icn"))) return "\x1b[35m";
    if (dot && !strcmp(dot, ".wav")) return "\x1b[36m";
    return 0;
}

static void list_dir(const char *path, bool show_header) {
    struct stat st;
    if (stat(path, &st) < 0) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return; }
    entry_t *ents = 0;
    int n = 0, cap = 0;
    if (!S_ISDIR(st.st_mode)) {
        ents = malloc(sizeof(entry_t));
        strlcpy(ents[0].name, path, 256);
        ents[0].st = st;
        n = 1;
    } else {
        DIR *d = opendir(path);
        if (!d) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return; }
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' && !opt_all) continue;
            if (n == cap) { cap = cap ? cap * 2 : 64; ents = realloc(ents, cap * sizeof(entry_t)); }
            strlcpy(ents[n].name, de->d_name, 256);
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
            if (stat(full, &ents[n].st) < 0) memset(&ents[n].st, 0, sizeof(struct stat));
            n++;
        }
        closedir(d);
    }
    qsort(ents, n, sizeof(entry_t), cmp);
    if (show_header) printf("%s:\n", path);
    bool tty = isatty(1);
    if (opt_long) {
        for (int i = 0; i < n; i++) {
            entry_t *e = &ents[i];
            char tbuf[32], sz[32];
            struct tm *tm = localtime(&e->st.st_mtime);
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", tm);
            if (opt_human) format_size(e->st.st_size, sz, sizeof(sz));
            else snprintf(sz, sizeof(sz), "%ld", (long)e->st.st_size);
            char type = S_ISDIR(e->st.st_mode) ? 'd' : S_ISCHR(e->st.st_mode) ? 'c' : '-';
            const char *c = tty ? color_for(e) : 0;
            printf("%c%s  %10s  %s  %s%s%s\n", type, S_ISDIR(e->st.st_mode) ? "rwxr-xr-x" : "rw-r--r--", sz, tbuf,
                   c ? c : "", e->name, c ? "\x1b[0m" : "");
        }
    } else if (opt_one || !tty) {
        for (int i = 0; i < n; i++) printf("%s\n", ents[i].name);
    } else {
        kwinsize_t ws;
        int cols = ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.cols ? ws.cols : 80;
        int maxw = 1;
        for (int i = 0; i < n; i++) maxw = MAX(maxw, (int)strlen(ents[i].name));
        int colw = maxw + 2;
        int per = MAX(1, cols / colw);
        int nrows = (n + per - 1) / per;
        for (int r = 0; r < nrows; r++) {
            for (int c = 0; c < per; c++) {
                int i = c * nrows + r;
                if (i >= n) continue;
                const char *col = color_for(&ents[i]);
                int pad = colw - (int)strlen(ents[i].name);
                printf("%s%s%s", col ? col : "", ents[i].name, col ? "\x1b[0m" : "");
                if (c < per - 1 && (c + 1) * nrows + r < n) printf("%*s", pad, "");
            }
            printf("\n");
        }
    }
    free(ents);
}

int main(int argc, char **argv) {
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (char *p = argv[first] + 1; *p; p++) {
            if (*p == 'l') opt_long = true;
            else if (*p == 'a') opt_all = true;
            else if (*p == '1') opt_one = true;
            else if (*p == 'h') opt_human = true;
        }
    }
    if (first >= argc) list_dir(".", false);
    for (int i = first; i < argc; i++) {
        list_dir(argv[i], argc - first > 1);
        if (argc - first > 1 && i < argc - 1) printf("\n");
    }
    return 0;
}
