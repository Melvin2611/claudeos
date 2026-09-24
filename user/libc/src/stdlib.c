#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

char **environ;
static char **env_owned;     /* environ copy we can modify */
static int env_count;

void __stdio_init(void);
void __stdio_flush_all(void);
int main(int argc, char **argv, char **envp);

#define MAX_ATEXIT 32
static void (*atexit_fns[MAX_ATEXIT])(void);
static int natexit;

int atexit(void (*fn)(void)) {
    if (natexit >= MAX_ATEXIT) return -1;
    atexit_fns[natexit++] = fn;
    return 0;
}

void _exit(int code) {
    syscall1(SYS_EXIT, code);
    for (;;) {}
}

void exit(int code) {
    while (natexit > 0) atexit_fns[--natexit]();
    __stdio_flush_all();
    _exit(code);
}

void abort(void) {
    static const char msg[] = "abort() called\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(134);
}

void __assert_fail(const char *expr, const char *file, int line) {
    fprintf(stderr, "Assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}

void __libc_start(int argc, char **argv, char **envp) {
    environ = envp;
    __stdio_init();
    exit(main(argc, argv, envp));
}

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { div_t d = { a / b, a % b }; return d; }

unsigned long long strtoull(const char *s, char **end, int base) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char)s[2])) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long long v = 0;
    const char *start = s;
    for (;;) {
        int d;
        if (isdigit((unsigned char)*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)(s == start ? start : s);
    return v;
}
long long strtoll(const char *s, char **end, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    char *e;
    unsigned long long v = strtoull(p, &e, base);
    if (e == p) { if (end) *end = (char *)s; return 0; }
    if (end) *end = e;
    return neg ? -(long long)v : (long long)v;
}
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    double v = 0;
    int digits = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; digits++; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (isdigit((unsigned char)*p)) { v += (*p - '0') * f; f *= 0.1; p++; digits++; }
    }
    if (!digits) { if (end) *end = (char *)s; return 0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, e = 0;
        if (*q == '+' || *q == '-') { eneg = *q == '-'; q++; }
        if (isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) { e = e * 10 + (*q - '0'); q++; }
            double m = 1;
            while (e--) m *= 10;
            v = eneg ? v / m : v * m;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}
double atof(const char *s) { return strtod(s, 0); }

static unsigned long rand_state = 1;
int rand(void) {
    rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((rand_state >> 33) & RAND_MAX);
}
void srand(unsigned seed) { rand_state = seed; }

static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *b = base;
    if (n < 2) return;
    if (n < 12) {
        for (size_t i = 1; i < n; i++)
            for (size_t j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; j--)
                swap_bytes(b + (j - 1) * size, b + j * size, size);
        return;
    }
    size_t mid = n / 2;
    swap_bytes(b + mid * size, b + (n - 1) * size, size);
    char *pivot = b + (n - 1) * size;
    size_t store = 0;
    for (size_t i = 0; i < n - 1; i++) {
        if (cmp(b + i * size, pivot) < 0) {
            swap_bytes(b + i * size, b + store * size, size);
            store++;
        }
    }
    swap_bytes(b + store * size, pivot, size);
    qsort(b, store, size, cmp);
    qsort(b + (store + 1) * size, n - store - 1, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const char *p = (const char *)base + mid * size;
        int c = cmp(key, p);
        if (c == 0) return (void *)p;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

char *getenv(const char *name) {
    if (!environ) return 0;
    size_t l = strlen(name);
    for (char **e = environ; *e; e++)
        if (!strncmp(*e, name, l) && (*e)[l] == '=') return *e + l + 1;
    return 0;
}

static void env_make_owned(void) {
    if (env_owned) return;
    int n = 0;
    while (environ && environ[n]) n++;
    env_owned = malloc(sizeof(char *) * (n + 16));
    for (int i = 0; i < n; i++) env_owned[i] = strdup(environ[i]);
    env_owned[n] = 0;
    env_count = n;
    environ = env_owned;
}

int setenv(const char *name, const char *value, int overwrite) {
    env_make_owned();
    size_t l = strlen(name);
    char *entry = malloc(l + strlen(value) + 2);
    strcpy(entry, name);
    entry[l] = '=';
    strcpy(entry + l + 1, value);
    for (int i = 0; i < env_count; i++) {
        if (!strncmp(env_owned[i], name, l) && env_owned[i][l] == '=') {
            if (!overwrite) { free(entry); return 0; }
            free(env_owned[i]);
            env_owned[i] = entry;
            return 0;
        }
    }
    env_owned = realloc(env_owned, sizeof(char *) * (env_count + 2));
    env_owned[env_count++] = entry;
    env_owned[env_count] = 0;
    environ = env_owned;
    return 0;
}

int unsetenv(const char *name) {
    env_make_owned();
    size_t l = strlen(name);
    for (int i = 0; i < env_count; i++) {
        if (!strncmp(env_owned[i], name, l) && env_owned[i][l] == '=') {
            free(env_owned[i]);
            env_owned[i] = env_owned[--env_count];
            env_owned[env_count] = 0;
            return 0;
        }
    }
    return 0;
}
