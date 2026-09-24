#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *wday_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *mon_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December" };

struct tm *gmtime_r(const time_t *tp, struct tm *tm) {
    long t = *tp;
    long days = t / 86400;
    long rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days + 4) % 7);
    if (tm->tm_wday < 0) tm->tm_wday += 7;
    /* civil from days (Howard Hinnant) */
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = (int)(m - 1);
    tm->tm_mday = (int)d;
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    tm->tm_yday = cum[tm->tm_mon] + tm->tm_mday - 1 + (leap && tm->tm_mon > 1);
    tm->tm_isdst = 0;
    return tm;
}

struct tm *gmtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }
/* the RTC runs in local time (like Windows), so localtime == gmtime */
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm) {
    long y = tm->tm_year + 1900, m = tm->tm_mon + 1;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + doe - 719468;
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    char tmp[64];
    for (; *fmt && n + 1 < max; fmt++) {
        if (*fmt != '%') { s[n++] = *fmt; continue; }
        fmt++;
        tmp[0] = 0;
        switch (*fmt) {
        case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
        case 'y': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100); break;
        case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
        case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 'p': strcpy(tmp, tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'A': strcpy(tmp, wday_names[tm->tm_wday]); break;
        case 'a': snprintf(tmp, 4, "%s", wday_names[tm->tm_wday]); break;
        case 'B': strcpy(tmp, mon_names[tm->tm_mon]); break;
        case 'b': snprintf(tmp, 4, "%s", mon_names[tm->tm_mon]); break;
        case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
        case 'F': snprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
        case 'T': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'R': snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
        case 'c': snprintf(tmp, sizeof(tmp), "%.3s %.3s %2d %02d:%02d:%02d %d", wday_names[tm->tm_wday],
                           mon_names[tm->tm_mon], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
                           tm->tm_year + 1900); break;
        case '%': strcpy(tmp, "%"); break;
        default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
        }
        for (char *t = tmp; *t && n + 1 < max; t++) s[n++] = *t;
    }
    s[n] = 0;
    return n;
}
