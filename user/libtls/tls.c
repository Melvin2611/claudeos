/* TLS client wrapper around BearSSL: sockets from the ClaudeOS network stack, entropy from
 * /dev/random, certificates verified against the compiled-in trust anchors. */
#include <bearssl.h>
#include <tls.h>
#include <net.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

extern const br_x509_trust_anchor *tls_trust_anchors;
extern const size_t tls_trust_anchors_num;

struct tls {
    int fd;
    br_ssl_client_context cc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    long r = read(*(int *)ctx, buf, len);
    return r > 0 ? (int)r : -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    long r = write(*(int *)ctx, buf, len);
    return r > 0 ? (int)r : -1;
}

static const char *tls_error_text(int e) {
    switch (e) {
    case BR_ERR_X509_EXPIRED: return "certificate expired or not yet valid (check the clock)";
    case BR_ERR_X509_NOT_TRUSTED: return "certificate not signed by a trusted authority";
    case BR_ERR_X509_BAD_SERVER_NAME: return "certificate does not match the host name";
    case BR_ERR_BAD_VERSION: return "unsupported TLS version";
    case BR_ERR_BAD_CIPHER_SUITE: return "no common cipher suite";
    case BR_ERR_IO: return "connection closed";
    default: return "handshake failed";
    }
}

tls_t *tls_connect(const char *host, uint16_t port, int timeout_ms, char *err, size_t errlen) {
    uint32_t ip;
    if (net_resolve(host, &ip) < 0) { snprintf(err, errlen, "cannot resolve %s", host); return 0; }
    int fd = socket(AF_INET, SOCK_STREAM);
    if (fd < 0 || connect_ip(fd, ip, port, timeout_ms) < 0) {
        snprintf(err, errlen, "cannot connect: %s", strerror(errno));
        if (fd >= 0) close(fd);
        return 0;
    }
    tls_t *t = calloc(1, sizeof(tls_t));
    if (!t) { close(fd); snprintf(err, errlen, "out of memory"); return 0; }
    t->fd = fd;
    br_ssl_client_init_full(&t->cc, &t->xc, tls_trust_anchors, tls_trust_anchors_num);
    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, sizeof(t->iobuf), 1);
    unsigned char seed[48];
    int rf = open("/dev/random", O_RDONLY);
    if (rf >= 0) { read(rf, seed, sizeof(seed)); close(rf); }
    br_ssl_engine_inject_entropy(&t->cc.eng, seed, sizeof(seed));
    br_ssl_client_reset(&t->cc, host, 0);
    br_sslio_init(&t->io, &t->cc.eng, sock_read, &t->fd, sock_write, &t->fd);
    /* drive the handshake */
    if (br_sslio_flush(&t->io) < 0 || (br_ssl_engine_current_state(&t->cc.eng) & BR_SSL_CLOSED)) {
        int e = br_ssl_engine_last_error(&t->cc.eng);
        snprintf(err, errlen, "TLS: %s (error %d)", tls_error_text(e), e);
        close(fd);
        free(t);
        return 0;
    }
    return t;
}

long tls_read(tls_t *t, void *buf, size_t len) {
    int r = br_sslio_read(&t->io, buf, len);
    if (r < 0) {
        int e = br_ssl_engine_last_error(&t->cc.eng);
        return (e == BR_ERR_OK || e == BR_ERR_IO) ? 0 : -1;     /* closed by the peer */
    }
    return r;
}

long tls_write(tls_t *t, const void *buf, size_t len) {
    if (br_sslio_write_all(&t->io, buf, len) < 0 || br_sslio_flush(&t->io) < 0) return -1;
    return (long)len;
}

void tls_close(tls_t *t) {
    if (!t) return;
    br_sslio_close(&t->io);
    close(t->fd);
    free(t);
}
