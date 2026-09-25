#include <net.h>
#include <stdio.h>
#include <errno.h>
#include <sys/syscall.h>

static long ret(long r) { if (r < 0) { errno = (int)-r; return -1; } return r; }

int socket(int domain, int type) { return (int)ret(syscall2(SYS_SOCKET, domain, type)); }
int connect_ip(int fd, uint32_t ip, uint16_t port, int timeout) { return (int)ret(syscall4(SYS_CONNECT, fd, ip, port, timeout)); }
int bind_port(int fd, uint16_t port) { return (int)ret(syscall2(SYS_BIND, fd, port)); }
long sendto_ip(int fd, const void *buf, size_t len, uint32_t ip, uint16_t port) {
    return ret(syscall5(SYS_SENDTO, fd, buf, len, ip, port));
}
long recvfrom_ip(int fd, void *buf, size_t len, uint32_t *ip, uint16_t *port, int timeout) {
    uint32_t from[2] = { 0, 0 };
    long r = ret(syscall5(SYS_RECVFROM, fd, buf, len, from, timeout));
    if (r >= 0) { if (ip) *ip = from[0]; if (port) *port = (uint16_t)from[1]; }
    return r;
}
int net_info(knetinfo_t *out) { return (int)ret(syscall1(SYS_NET_INFO, out)); }
int net_ping(uint32_t ip, uint16_t seq, int timeout) { return (int)ret(syscall3(SYS_NET_PING, ip, seq, timeout)); }
int net_resolve(const char *name, uint32_t *ip) { return (int)ret(syscall2(SYS_NET_RESOLVE, name, ip)); }

void ip_to_str(uint32_t ip, char *out, size_t n) {
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

int str_to_ip(const char *s, uint32_t *ip) {
    uint32_t v[4];
    int n = 0;
    while (n < 4) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t x = 0;
        while (*s >= '0' && *s <= '9') x = x * 10 + (uint32_t)(*s++ - '0');
        if (x > 255) return -1;
        v[n++] = x;
        if (n < 4) { if (*s != '.') return -1; s++; }
    }
    if (*s) return -1;
    *ip = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
    return 0;
}
