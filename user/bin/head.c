/* head / tail - first or last lines of a file (-n N) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    int tail = !strcmp(name, "tail");
    int n = 10, i = 1;
    if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) { n = atoi(argv[i + 1]); i += 2; }
    else if (i < argc && argv[i][0] == '-' && argv[i][1]) { n = atoi(argv[i] + 1); i++; }
    FILE *f = i < argc ? fopen(argv[i], "r") : stdin;
    if (!f) { fprintf(stderr, "%s: %s: %s\n", name, argv[i], strerror(errno)); return 1; }
    char *line = 0;
    size_t cap = 0;
    if (!tail) {
        for (int k = 0; k < n && getline(&line, &cap, f) > 0; k++) fputs(line, stdout);
    } else {
        char **ring = calloc(n > 0 ? n : 1, sizeof(char *));
        long count = 0;
        while (getline(&line, &cap, f) > 0) {
            if (n <= 0) continue;
            free(ring[count % n]);
            ring[count % n] = strdup(line);
            count++;
        }
        long start = count > n ? count - n : 0;
        for (long k = start; k < count; k++) fputs(ring[k % n], stdout);
    }
    if (f != stdin) fclose(f);
    return 0;
}
