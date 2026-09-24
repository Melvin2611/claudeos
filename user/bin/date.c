/* date - print (or set: date -s "YYYY-MM-DD HH:MM[:SS]") the date */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "-s")) {
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        int y, mo, d, h, mi, s = 0;
        if (sscanf(argv[2], "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 5) {
            fprintf(stderr, "date: invalid date, use \"YYYY-MM-DD HH:MM[:SS]\"\n");
            return 1;
        }
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d; tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
        set_time((uint64_t)mktime(&tm));
    }
    time_t t = time(0);
    char buf[64];
    const char *fmt = "%A, %d %B %Y  %H:%M:%S";
    if (argc > 1 && argv[1][0] == '+') fmt = argv[1] + 1;
    strftime(buf, sizeof(buf), fmt, localtime(&t));
    puts(buf);
    return 0;
}
