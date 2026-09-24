/* printf-style formatting engine, shared by kernel (no floats) and userland (FMT_FLOAT). */
#include <claudeos/fmt.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    fmt_out_fn out;
    void *ctx;
    int count;
} fmt_state_t;

static void emit(fmt_state_t *st, char c) {
    st->out(c, st->ctx);
    st->count++;
}

static void emit_padded(fmt_state_t *st, const char *s, int len, int width, bool left, char pad,
                        const char *prefix) {
    int plen = 0;
    if (prefix) while (prefix[plen]) plen++;
    int total = len + plen;
    if (!left && pad == ' ')
        for (int i = total; i < width; i++) emit(st, ' ');
    for (int i = 0; i < plen; i++) emit(st, prefix[i]);
    if (!left && pad == '0')
        for (int i = total; i < width; i++) emit(st, '0');
    for (int i = 0; i < len; i++) emit(st, s[i]);
    if (left)
        for (int i = total; i < width; i++) emit(st, ' ');
}

static int utoa_buf(char *buf, unsigned long long v, int base, bool upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

#ifdef FMT_FLOAT
/* Format a double in %f / %e / %g style into buf. Returns length. */
static int fmt_double(char *buf, double v, int prec, char spec, bool alt) {
    int n = 0;
    if (v != v) { buf[0] = 'n'; buf[1] = 'a'; buf[2] = 'n'; return 3; }
    if (v < 0) { buf[n++] = '-'; v = -v; }
    if (v > 1.7976931348623157e308) { buf[n++] = 'i'; buf[n++] = 'n'; buf[n++] = 'f'; return n; }
    if (prec < 0) prec = 6;
    if (prec > 17) prec = 17;

    int exp10 = 0;
    bool use_exp = (spec == 'e' || spec == 'E');
    bool strip = false;
    if (spec == 'g' || spec == 'G') {
        if (prec == 0) prec = 1;
        double t = v;
        if (t != 0) {
            while (t >= 10.0) { t /= 10.0; exp10++; }
            while (t < 1.0) { t *= 10.0; exp10--; }
        }
        /* rounding may bump exponent */
        double r = 0.5;
        for (int i = 0; i < prec - 1; i++) r /= 10.0;
        if (t + r >= 10.0) exp10++;
        if (exp10 < -4 || exp10 >= prec) { use_exp = true; prec = prec - 1; }
        else { prec = prec - 1 - exp10; }
        strip = !alt;
    }

    if (use_exp) {
        exp10 = 0;
        if (v != 0) {
            while (v >= 10.0) { v /= 10.0; exp10++; }
            while (v < 1.0) { v *= 10.0; exp10--; }
        }
        double r = 0.5;
        for (int i = 0; i < prec; i++) r /= 10.0;
        v += r;
        if (v >= 10.0) { v /= 10.0; exp10++; }
        int d = (int)v;
        buf[n++] = '0' + d;
        v -= d;
        int start_frac = n;
        if (prec > 0 || alt) buf[n++] = '.';
        for (int i = 0; i < prec; i++) {
            v *= 10.0;
            d = (int)v;
            if (d > 9) d = 9;
            buf[n++] = '0' + d;
            v -= d;
        }
        if (strip && n > start_frac) {
            while (buf[n - 1] == '0') n--;
            if (buf[n - 1] == '.') n--;
        }
        buf[n++] = (spec == 'E' || spec == 'G') ? 'E' : 'e';
        if (exp10 < 0) { buf[n++] = '-'; exp10 = -exp10; } else buf[n++] = '+';
        if (exp10 >= 100) { buf[n++] = '0' + exp10 / 100; exp10 %= 100; }
        buf[n++] = '0' + exp10 / 10;
        buf[n++] = '0' + exp10 % 10;
        return n;
    }

    /* fixed notation */
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10.0;
    v += r;
    if (v >= 1e19) {
        /* too large for integer part conversion: fall back to exponent */
        return fmt_double(buf, v, prec, 'e', alt);
    }
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    n += utoa_buf(buf + n, ip, 10, false);
    int start_frac = n;
    if (prec > 0 || alt) buf[n++] = '.';
    for (int i = 0; i < prec; i++) {
        frac *= 10.0;
        int d = (int)frac;
        if (d > 9) d = 9;
        buf[n++] = '0' + d;
        frac -= d;
    }
    if (strip && n > start_frac) {
        while (buf[n - 1] == '0') n--;
        if (buf[n - 1] == '.') n--;
    }
    return n;
}
#endif

int fmt_format(fmt_out_fn out, void *ctx, const char *fmt, va_list ap) {
    fmt_state_t st = { out, ctx, 0 };
    char buf[400];

    while (*fmt) {
        if (*fmt != '%') { emit(&st, *fmt++); continue; }
        fmt++;
        bool left = false, plus = false, space = false, alt = false;
        char pad = ' ';
        for (;;) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') pad = '0';
            else if (*fmt == '+') plus = true;
            else if (*fmt == ' ') space = true;
            else if (*fmt == '#') alt = true;
            else break;
            fmt++;
        }
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;   /* 0 int, 1 long, 2 long long, -1 short, -2 char, 3 size_t */
        for (;;) {
            if (*fmt == 'l') { lng = lng == 1 ? 2 : 1; fmt++; }
            else if (*fmt == 'h') { lng = lng == -1 ? -2 : -1; fmt++; }
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 3; fmt++; }
            else break;
        }
        if (left) pad = ' ';
        char spec = *fmt++;
        switch (spec) {
        case 'd': case 'i': {
            long long v;
            if (lng == 2 || lng == 1 || lng == 3) v = va_arg(ap, long long);
            else v = va_arg(ap, int);
            if (lng == -1) v = (short)v;
            if (lng == -2) v = (signed char)v;
            const char *prefix = 0;
            unsigned long long uv;
            if (v < 0) { prefix = "-"; uv = (unsigned long long)(-(v + 1)) + 1; }
            else { uv = v; if (plus) prefix = "+"; else if (space) prefix = " "; }
            int len = utoa_buf(buf, uv, 10, false);
            if (prec >= 0) {
                pad = ' ';
                if (len < prec) {
                    int shift = prec - len;
                    for (int i = len - 1; i >= 0; i--) buf[i + shift] = buf[i];
                    for (int i = 0; i < shift; i++) buf[i] = '0';
                    len = prec;
                }
                if (prec == 0 && uv == 0) len = 0;
            }
            emit_padded(&st, buf, len, width, left, pad, prefix);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'b': {
            unsigned long long v;
            if (lng == 2 || lng == 1 || lng == 3) v = va_arg(ap, unsigned long long);
            else v = va_arg(ap, unsigned int);
            if (lng == -1) v = (unsigned short)v;
            if (lng == -2) v = (unsigned char)v;
            int base = spec == 'u' ? 10 : spec == 'o' ? 8 : spec == 'b' ? 2 : 16;
            int len = utoa_buf(buf, v, base, spec == 'X');
            if (prec >= 0) {
                pad = ' ';
                if (len < prec) {
                    int shift = prec - len;
                    for (int i = len - 1; i >= 0; i--) buf[i + shift] = buf[i];
                    for (int i = 0; i < shift; i++) buf[i] = '0';
                    len = prec;
                }
            }
            const char *prefix = 0;
            if (alt && v != 0) prefix = spec == 'x' ? "0x" : spec == 'X' ? "0X" : spec == 'o' ? "0" : 0;
            emit_padded(&st, buf, len, width, left, pad, prefix);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            int len = utoa_buf(buf, v, 16, false);
            emit_padded(&st, buf, len, width, left, pad, "0x");
            break;
        }
        case 'c': {
            buf[0] = (char)va_arg(ap, int);
            emit_padded(&st, buf, 1, width, left, ' ', 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len] && (prec < 0 || len < prec)) len++;
            emit_padded(&st, s, len, width, left, ' ', 0);
            break;
        }
#ifdef FMT_FLOAT
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = va_arg(ap, double);
            int len = fmt_double(buf, v, prec, spec, alt);
            const char *prefix = 0;
            const char *s = buf;
            if (buf[0] == '-') { prefix = "-"; s = buf + 1; len--; }
            else if (plus) prefix = "+";
            else if (space) prefix = " ";
            emit_padded(&st, s, len, width, left, pad, prefix);
            break;
        }
#endif
        case '%':
            emit(&st, '%');
            break;
        case 0:
            return st.count;
        default:
            emit(&st, '%');
            emit(&st, spec);
            break;
        }
    }
    return st.count;
}
