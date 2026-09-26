#pragma once
/* TLS client connections (BearSSL underneath) */
#include <stddef.h>
#include <stdint.h>

typedef struct tls tls_t;

/* connect to host:port and complete the TLS handshake (certificate checked against the
 * built-in root CAs). On failure returns NULL and describes the problem in err. */
tls_t *tls_connect(const char *host, uint16_t port, int timeout_ms, char *err, size_t errlen);
long tls_read(tls_t *t, void *buf, size_t len);          /* 0 = closed, <0 = error */
long tls_write(tls_t *t, const void *buf, size_t len);   /* writes everything or fails */
void tls_close(tls_t *t);
