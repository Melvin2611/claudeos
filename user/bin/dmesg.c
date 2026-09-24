/* dmesg - print the kernel log */
#include <stdio.h>
#include <unistd.h>
#include <claudeos.h>

int main(void) {
    char buf[4096];
    size_t off = 0;
    long n;
    while ((n = dmesg(buf, sizeof(buf), off)) > 0) {
        write(1, buf, n);
        off += n;
    }
    return 0;
}
