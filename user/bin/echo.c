/* echo - print arguments */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int i = 1, nl = 1;
    if (argc > 1 && !strcmp(argv[1], "-n")) { nl = 0; i++; }
    for (; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i < argc - 1) putchar(' ');
    }
    if (nl) putchar('\n');
    return 0;
}
