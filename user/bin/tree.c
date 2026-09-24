/* tree - show a directory tree */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static int dirs, files;

static int cmp(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

static void walk(const char *path, const char *prefix) {
    DIR *d = opendir(path);
    if (!d) return;
    char *names[512];
    unsigned char types[512];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 512) {
        if (e->d_name[0] == '.') continue;
        names[n] = strdup(e->d_name);
        types[n] = e->d_type;
        n++;
    }
    closedir(d);
    /* sort names, keep types aligned */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char *t = names[i]; names[i] = names[j]; names[j] = t;
                unsigned char tt = types[i]; types[i] = types[j]; types[j] = tt;
            }
    for (int i = 0; i < n; i++) {
        bool last = i == n - 1;
        printf("%s%s", prefix, last ? "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 ");
        if (types[i] == DT_DIR) {
            printf("\x1b[1;34m%s\x1b[0m\n", names[i]);
            dirs++;
            char sub[512], pre[256];
            snprintf(sub, sizeof(sub), "%s/%s", path, names[i]);
            snprintf(pre, sizeof(pre), "%s%s", prefix, last ? "    " : "\xE2\x94\x82   ");
            walk(sub, pre);
        } else {
            printf("%s\n", names[i]);
            files++;
        }
        free(names[i]);
    }
    (void)cmp;
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    printf("\x1b[1;34m%s\x1b[0m\n", root);
    walk(root, "");
    printf("\n%d directories, %d files\n", dirs, files);
    return 0;
}
