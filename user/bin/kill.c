/* kill - terminate processes (kill [-9] pid... | kill name) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    int sig = SIGTERM, i = 1, rc = 0;
    if (i < argc && argv[i][0] == '-') { sig = atoi(argv[i] + 1); if (!sig) sig = SIGKILL; i++; }
    if (i >= argc) { fprintf(stderr, "usage: kill [-9] pid|name...\n"); return 1; }
    for (; i < argc; i++) {
        if (isdigit((unsigned char)argv[i][0])) {
            if (kill(atoi(argv[i]), sig) < 0) { fprintf(stderr, "kill: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
            continue;
        }
        /* by name */
        kprocinfo_t p;
        int found = 0;
        for (int k = 0; proc_info(k, &p) > 0; k++) {
            if (!p.is_kernel && !strcmp(p.name, argv[i]) && kill(p.pid, sig) == 0) found++;
        }
        if (!found) { fprintf(stderr, "kill: no process named %s\n", argv[i]); rc = 1; }
    }
    return rc;
}
