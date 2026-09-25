/* ping - send ICMP echo requests */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <net.h>
#include <claudeos.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ping host [count]\n"); return 1; }
    int count = argc > 2 ? atoi(argv[2]) : 4;
    uint32_t ip;
    if (net_resolve(argv[1], &ip) < 0) { fprintf(stderr, "ping: cannot resolve %s: %s\n", argv[1], strerror(errno)); return 1; }
    char s[20];
    ip_to_str(ip, s, sizeof(s));
    printf("PING %s (%s): 56 data bytes\n", argv[1], s);
    int ok = 0;
    long total = 0, mn = -1, mx = 0;
    for (int i = 0; i < count; i++) {
        int us = net_ping(ip, (uint16_t)(i + 1), 2000);
        if (us < 0) printf("Request timeout for icmp_seq %d (%s)\n", i + 1, strerror(errno));
        else {
            printf("64 bytes from %s: icmp_seq=%d time=%d.%03d ms\n", s, i + 1, us / 1000, us % 1000);
            ok++;
            total += us;
            if (mn < 0 || us < mn) mn = us;
            if (us > mx) mx = us;
        }
        fflush(stdout);
        if (i < count - 1) msleep(1000);
    }
    printf("--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss\n", argv[1], count, ok,
           count ? (count - ok) * 100 / count : 0);
    if (ok) printf("round-trip min/avg/max = %.3f/%.3f/%.3f ms\n", mn / 1000.0, total / ok / 1000.0, mx / 1000.0);
    return ok ? 0 : 1;
}
