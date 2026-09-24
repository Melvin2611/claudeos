#include <kernel.h>
#include <claudeos/fmt.h>

typedef struct { char *buf; size_t n, pos; } sbuf_t;

static void sbuf_out(char c, void *ctx) {
    sbuf_t *s = ctx;
    if (s->pos + 1 < s->n) s->buf[s->pos] = c;
    s->pos++;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    sbuf_t s = { buf, n, 0 };
    fmt_format(sbuf_out, &s, fmt, ap);
    if (n) buf[s.pos < n ? s.pos : n - 1] = 0;
    return (int)s.pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
