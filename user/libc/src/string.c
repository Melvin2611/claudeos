#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>

void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = d;
    const unsigned char *ss = s;
    if (dd == ss || !n) return d;
    if (dd < ss || dd >= ss + n) return memcpy(d, s, n);
    dd += n - 1;
    ss += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dd), "+S"(ss), "+c"(n) :: "memory");
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i);
    return 0;
}

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    return 0;
}
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n) {
    char *e = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) { size_t c = len >= n ? n - 1 : len; memcpy(d, s, c); d[c] = 0; }
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
    for (; *h; h++) if (*h == *n && !strncmp(h, n, nl)) return (char *)h;
    return 0;
}
char *strcasestr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (!strncasecmp(h, n, nl)) return (char *)h;
    return 0;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
char *strndup(const char *s, size_t n) {
    size_t l = strnlen(s, n);
    char *d = malloc(l + 1);
    if (d) { memcpy(d, s, l); d[l] = 0; }
    return d;
}
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *a) { s += strcspn(s, a); return *s ? (char *)s : 0; }
char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) { *save = 0; return 0; }
    char *e = s + strcspn(s, delim);
    if (*e) { *e = 0; *save = e + 1; } else *save = 0;
    return s;
}
char *strtok(char *s, const char *delim) { static char *save; return strtok_r(s, delim, &save); }

char *strerror(int e) {
    switch (e) {
    case 0: return "Success";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case ESRCH: return "No such process";
    case EINTR: return "Interrupted";
    case EIO: return "Input/output error";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Exec format error";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
    case EAGAIN: return "Resource temporarily unavailable";
    case ENOMEM: return "Out of memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case EXDEV: return "Cross-device link";
    case ENODEV: return "No such device";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case ENOTTY: return "Not a terminal";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EROFS: return "Read-only file system";
    case EPIPE: return "Broken pipe";
    case ERANGE: return "Result out of range";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Function not implemented";
    case ENOTEMPTY: return "Directory not empty";
    case ETIMEDOUT: return "Connection timed out";
    case ECONNREFUSED: return "Connection refused";
    case EHOSTUNREACH: return "No route to host";
    case ENETUNREACH: return "Network is unreachable";
    case ENETDOWN: return "Network is down";
    case ECONNRESET: return "Connection reset by peer";
    case ENOTCONN: return "Not connected";
    default: return "Unknown error";
    }
}

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
