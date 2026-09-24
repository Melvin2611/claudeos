/* uptime */
#include <stdio.h>
#include <time.h>
#include <claudeos.h>

int main(void) {
    ksysinfo_t si;
    sys_info(&si);
    unsigned long s = (unsigned long)(si.uptime_ms / 1000);
    time_t now = time(0);
    char tb[16];
    strftime(tb, sizeof(tb), "%H:%M:%S", localtime(&now));
    printf(" %s up ", tb);
    if (s >= 86400) printf("%lu day%s, ", s / 86400, s / 86400 == 1 ? "" : "s");
    printf("%lu:%02lu:%02lu, %u processes\n", s / 3600 % 24, s / 60 % 60, s % 60, si.nprocs);
    return 0;
}
