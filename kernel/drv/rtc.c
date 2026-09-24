/* CMOS real time clock -> wall clock time */
#include <kernel.h>
#include "drivers.h"

static uint64_t boot_epoch;      /* unix time at uptime 0 */
static int64_t time_adjust;      /* seconds added by settime */

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static int updating(void) { return cmos(0x0A) & 0x80; }

static uint64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)(era * 146097 + (int)doe - 719468);
}

uint64_t rtc_read_epoch(void) {
    uint8_t s, m, h, d, mo, y, c = 0, regb;
    for (int tries = 0; tries < 5; tries++) {
        while (updating()) cpu_pause();
        s = cmos(0); m = cmos(2); h = cmos(4); d = cmos(7); mo = cmos(8); y = cmos(9); c = cmos(0x32);
        if (!updating() && s == cmos(0)) break;
    }
    regb = cmos(0x0B);
    bool bcd = !(regb & 4);
    bool pm = h & 0x80;
    h &= 0x7F;
#define B2D(v) ((v & 0x0F) + ((v >> 4) * 10))
    if (bcd) { s = B2D(s); m = B2D(m); h = B2D(h); d = B2D(d); mo = B2D(mo); y = B2D(y); c = B2D(c); }
    if (!(regb & 2) && pm) h = (h + 12) % 24;
    int year = (c >= 19 && c <= 21) ? c * 100 + y : 2000 + y;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    return days_from_civil(year, mo, d) * 86400 + h * 3600 + m * 60 + s;
}

void rtc_init(void) {
    uint64_t now = rtc_read_epoch();
    boot_epoch = now - uptime_ms() / 1000;
    klog("[rtc] wall clock: %lu (unix)\n", now);
}

uint64_t time_now(void) { return boot_epoch + uptime_ms() / 1000 + time_adjust; }

void time_set(uint64_t epoch) { time_adjust = (int64_t)epoch - (int64_t)(boot_epoch + uptime_ms() / 1000); }
