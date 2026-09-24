#include <kernel.h>

void *memset(void *d, int c, size_t n) {
    void *ret = d;
    if (n >= 16 && ((uintptr_t)d & 7) == 0) {
        uint64_t v = (uint8_t)c;
        v |= v << 8; v |= v << 16; v |= v << 32;
        size_t q = n / 8;
        __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(v) : "memory");
        n &= 7;
    }
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return ret;
}

void *memcpy(void *d, const void *s, size_t n) {
    void *ret = d;
    if (n >= 16 && (((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
        size_t q = n / 8;
        __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) :: "memory");
        n &= 7;
    }
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return ret;
}

void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d;
    const uint8_t *ss = s;
    if (dd == ss || n == 0) return d;
    if (dd < ss || dd >= ss + n) return memcpy(d, s, n);
    /* overlapping, copy backwards */
    dd += n - 1;
    ss += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dd), "+S"(ss), "+c"(n) :: "memory");
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (uint8_t)a[i] - (uint8_t)b[i];
    }
    return 0;
}
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((uint8_t)*a) - tolower((uint8_t)*b);
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t c = len >= n ? n - 1 : len;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}
size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c == 0 ? (char *)s : 0;
}
char *strrchr(const char *s, int c) {
    const char *r = 0;
    for (; *s; s++) if (*s == (char)c) r = s;
    return c == 0 ? (char *)s : (char *)r;
}
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, nl)) return (char *)h;
    return 0;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

unsigned long strtoul(const char *s, char **end, int base) {
    while (isspace(*s)) s++;
    if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long v = 0;
    for (;;) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return v;
}
long strtol(const char *s, char **end, int base) {
    while (isspace(*s)) s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    unsigned long v = strtoul(s, end, base);
    return neg ? -(long)v : (long)v;
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
