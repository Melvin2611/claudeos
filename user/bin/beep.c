/* beep [frequency] [ms] - PC speaker tone */
#include <stdlib.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    unsigned f = argc > 1 ? (unsigned)atoi(argv[1]) : 880;
    unsigned ms = argc > 2 ? (unsigned)atoi(argv[2]) : 200;
    return beep(f, ms) < 0;
}
