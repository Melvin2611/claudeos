/* sleep - pause for N seconds (fractions allowed) */
#include <stdio.h>
#include <stdlib.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sleep seconds\n"); return 1; }
    double s = strtod(argv[1], 0);
    msleep((unsigned)(s * 1000));
    return 0;
}
