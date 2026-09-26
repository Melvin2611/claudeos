/* wget - download a file over HTTP/1.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <net.h>
#include <claudeos.h>
#include <tls.h>

/* one connection: plain TCP or TLS */
typedef struct { int fd; tls_t *tls; } conn_t;
static long c_read(conn_t *c, void *b, size_t n) { return c->tls ? tls_read(c->tls, b, n) : read(c->fd, b, n); }
static long c_write(conn_t *c, const void *b, size_t n) { return c->tls ? tls_write(c->tls, b, n) : write(c->fd, b, n); }
static void c_close(conn_t *c) { if (c->tls) tls_close(c->tls); else close(c->fd); }

static int fetch(const char *url, const char *out_name, int redirects) {
    bool https = !strncmp(url, "https://", 8);
    if (!https && strncmp(url, "http://", 7)) {
        fprintf(stderr, "wget: only http:// and https:// URLs are supported\n");
        return 1;
    }
    char host[128], path[512] = "/";
    const char *h = url + (https ? 8 : 7);
    const char *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl >= sizeof(host)) hl = sizeof(host) - 1;
    memcpy(host, h, hl);
    host[hl] = 0;
    if (slash) strlcpy(path, slash, sizeof(path));
    uint16_t port = https ? 443 : 80;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }
    uint32_t ip;
    printf("Resolving %s... ", host);
    fflush(stdout);
    if (net_resolve(host, &ip) < 0) { printf("failed: %s\n", strerror(errno)); return 1; }
    char ips[20];
    ip_to_str(ip, ips, sizeof(ips));
    printf("%s\nConnecting to %s:%u... ", ips, ips, port);
    fflush(stdout);
    conn_t conn = { -1, 0 };
    conn_t *fd = &conn;
    if (https) {
        char err[160];
        conn.tls = tls_connect(host, port, 10000, err, sizeof(err));
        if (!conn.tls) { printf("failed: %s\n", err); return 1; }
        printf("connected (TLS, certificate verified).\n");
    } else {
        conn.fd = socket(AF_INET, SOCK_STREAM);
        if (conn.fd < 0 || connect_ip(conn.fd, ip, port, 10000) < 0) { printf("failed: %s\n", strerror(errno)); return 1; }
        printf("connected.\n");
    }
    char req[800];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ClaudeOS-wget/1.0\r\nConnection: close\r\n\r\n", path, host);
    c_write(fd, req, n);
    /* read the header */
    char hdr[8192];
    size_t hlen = 0;
    char *body = 0;
    while (hlen < sizeof(hdr) - 1) {
        long r = c_read(fd, hdr + hlen, sizeof(hdr) - 1 - hlen);
        if (r <= 0) break;
        hlen += r;
        hdr[hlen] = 0;
        if ((body = strstr(hdr, "\r\n\r\n"))) break;
    }
    if (!body) { printf("Invalid HTTP response\n"); c_close(fd); return 1; }
    *body = 0;
    body += 4;
    size_t pre = hlen - (size_t)(body - hdr);
    int status = 0;
    char *sp = strchr(hdr, ' ');
    if (sp) status = atoi(sp + 1);
    printf("HTTP request sent, response: %d\n", status);
    if ((status == 301 || status == 302 || status == 303 || status == 307) && redirects < 5) {
        char *loc = strstr(hdr, "\nLocation:");
        if (!loc) loc = strstr(hdr, "\nlocation:");
        if (loc) {
            loc += 10;
            while (*loc == ' ') loc++;
            char *e = strpbrk(loc, "\r\n");
            if (e) *e = 0;
            printf("Redirect to %s\n", loc);
            c_close(fd);
            return fetch(loc, out_name, redirects + 1);
        }
    }
    long total = -1;
    char *cl = strcasestr(hdr, "Content-Length:");
    if (cl) total = atol(cl + 15);
    char name[256];
    if (out_name) strlcpy(name, out_name, sizeof(name));
    else {
        const char *base = strrchr(path, '/');
        strlcpy(name, base && base[1] ? base + 1 : "index.html", sizeof(name));
        char *q = strchr(name, '?');
        if (q) *q = 0;
    }
    int out = open(name, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { printf("cannot create %s: %s\n", name, strerror(errno)); c_close(fd); return 1; }
    long got = 0;
    if (pre) { write(out, body, pre); got += pre; }
    char buf[4096];
    uint64_t t0 = uptime_ms();
    for (;;) {
        long r = c_read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        write(out, buf, r);
        got += r;
        if (total > 0) printf("\r%s  %ld / %ld bytes (%ld%%)", name, got, total, got * 100 / total);
        else printf("\r%s  %ld bytes", name, got);
        fflush(stdout);
    }
    close(out);
    c_close(fd);
    uint64_t ms = uptime_ms() - t0 + 1;
    printf("\nSaved '%s' (%ld bytes, %.1f KB/s)\n", name, got, got / 1.024 / ms);
    return status >= 200 && status < 300 ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *out = 0, *url = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc) out = argv[++i];
        else url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: wget [-O file] http[s]://host/path\n"); return 1; }
    return fetch(url, out, 0);
}
