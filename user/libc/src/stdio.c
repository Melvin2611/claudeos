/* buffered stdio on top of file descriptors */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <claudeos/fmt.h>

#define F_READ 1
#define F_WRITE 2
#define F_EOF 4
#define F_ERR 8

static char stdin_buf[BUFSIZ], stdout_buf[BUFSIZ];
static FILE f_stdin = { 0, F_READ, _IOLBF, stdin_buf, BUFSIZ, 0, 0, -1, 0 };
static FILE f_stdout = { 1, F_WRITE, _IOLBF, stdout_buf, BUFSIZ, 0, 0, -1, 0 };
static FILE f_stderr = { 2, F_WRITE, _IONBF, 0, 0, 0, 0, -1, 0 };
FILE *stdin = &f_stdin, *stdout = &f_stdout, *stderr = &f_stderr;

#define MAX_OPEN 64
static FILE *open_files[MAX_OPEN];

void __stdio_init(void) {
    if (!isatty(1)) f_stdout.mode = _IOFBF;
}

void __stdio_flush_all(void) {
    fflush(stdout);
    fflush(stderr);
    for (int i = 0; i < MAX_OPEN; i++) if (open_files[i]) fflush(open_files[i]);
}

static int parse_mode(const char *mode, int *fl) {
    int flags = 0, acc = 0;
    switch (mode[0]) {
    case 'r': acc = F_READ; flags = O_RDONLY; break;
    case 'w': acc = F_WRITE; flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': acc = F_WRITE; flags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: return -1;
    }
    if (strchr(mode, '+')) { acc = F_READ | F_WRITE; flags = (flags & ~O_ACCMODE) | O_RDWR; }
    *fl = flags;
    return acc;
}

static FILE *mkfile(int fd, int acc) {
    FILE *f = calloc(1, sizeof(FILE));
    if (!f) return 0;
    f->fd = fd;
    f->flags = acc;
    f->mode = _IOFBF;
    f->bufsize = BUFSIZ;
    f->buf = malloc(BUFSIZ);
    f->ungot = -1;
    for (int i = 0; i < MAX_OPEN; i++) if (!open_files[i]) { open_files[i] = f; break; }
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int flags;
    int acc = parse_mode(mode, &flags);
    if (acc < 0) { errno = EINVAL; return 0; }
    int fd = open(path, flags);
    if (fd < 0) return 0;
    FILE *f = mkfile(fd, acc);
    if (!f) close(fd);
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    int flags;
    int acc = parse_mode(mode, &flags);
    if (acc < 0) return 0;
    return mkfile(fd, acc);
}

int fflush(FILE *f);
static int fflush_unlocked(FILE *f) {
    if (!f) { __stdio_flush_all(); return 0; }
    if (f->last_write && f->len) {
        size_t off = 0;
        while (off < f->len) {
            ssize_t w = write(f->fd, f->buf + off, f->len - off);
            if (w <= 0) { f->flags |= F_ERR; f->len = 0; return EOF; }
            off += w;
        }
        f->len = 0;
    } else if (!f->last_write && f->len > f->pos) {
        /* discard read-ahead: move the file position back */
        lseek(f->fd, -(long)(f->len - f->pos), SEEK_CUR);
        f->pos = f->len = 0;
    }
    return 0;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    fflush(f);
    int r = close(f->fd);
    for (int i = 0; i < MAX_OPEN; i++) if (open_files[i] == f) open_files[i] = 0;
    if (f != stdin && f != stdout && f != stderr) {
        free(f->buf);
        free(f);
    }
    return r;
}

static int fill(FILE *f) {
    if (f->last_write) { fflush(f); f->last_write = 0; }
    if (!f->buf || f->mode == _IONBF) return 0;
    if (f == stdin) fflush(stdout);
    ssize_t r = read(f->fd, f->buf, f->bufsize);
    if (r < 0) { f->flags |= F_ERR; return -1; }
    if (r == 0) { f->flags |= F_EOF; return 0; }
    f->pos = 0;
    f->len = r;
    return (int)r;
}

int fgetc(FILE *f) {
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->last_write) { fflush(f); f->last_write = 0; f->pos = f->len = 0; }
    if (!f->buf || f->mode == _IONBF) {
        unsigned char c;
        ssize_t r = read(f->fd, &c, 1);
        if (r <= 0) { f->flags |= r == 0 ? F_EOF : F_ERR; return EOF; }
        return c;
    }
    if (f->pos >= f->len && fill(f) <= 0) return EOF;
    return (unsigned char)f->buf[f->pos++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int c, FILE *f) { if (c == EOF) return EOF; f->ungot = c; f->flags &= ~F_EOF; return c; }

char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    s[i] = 0;
    return s;
}

long getline(char **line, size_t *cap, FILE *f) {
    if (!*line || !*cap) { *cap = 128; *line = malloc(*cap); }
    size_t n = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (n + 2 > *cap) { *cap *= 2; *line = realloc(*line, *cap); }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0) return -1;
    (*line)[n] = 0;
    return (long)n;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f) {
    size_t total = size * n, got = 0;
    unsigned char *b = buf;
    if (f->ungot >= 0 && total) { b[got++] = (unsigned char)f->ungot; f->ungot = -1; }
    if (f->last_write) { fflush(f); f->last_write = 0; f->pos = f->len = 0; }
    /* drain buffer */
    while (got < total && f->pos < f->len) b[got++] = f->buf[f->pos++];
    while (got < total) {
        ssize_t r = read(f->fd, b + got, total - got);
        if (r < 0) { f->flags |= F_ERR; break; }
        if (r == 0) { f->flags |= F_EOF; break; }
        got += r;
    }
    return size ? got / size : 0;
}

static int flush_if_needed(FILE *f) {
    if (!f->last_write) {
        if (f->len > f->pos) lseek(f->fd, -(long)(f->len - f->pos), SEEK_CUR);
        f->pos = f->len = 0;
        f->last_write = 1;
    }
    return 0;
}

static size_t fwrite_unlocked(const void *buf, size_t size, size_t n, FILE *f) {
    size_t total = size * n;
    const char *b = buf;
    flush_if_needed(f);
    if (!f->buf || f->mode == _IONBF) {
        size_t off = 0;
        while (off < total) {
            ssize_t w = write(f->fd, b + off, total - off);
            if (w <= 0) { f->flags |= F_ERR; break; }
            off += w;
        }
        return size ? off / size : 0;
    }
    for (size_t i = 0; i < total; i++) {
        if (f->len >= f->bufsize) fflush(f), f->last_write = 1;
        f->buf[f->len++] = b[i];
        if (f->mode == _IOLBF && b[i] == '\n') { fflush(f); f->last_write = 1; }
    }
    return n;
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? ch : EOF;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { size_t l = strlen(s); return fwrite(s, 1, l, f) == l ? 0 : EOF; }
int puts(const char *s) { fputs(s, stdout); return fputc('\n', stdout) == EOF ? EOF : 0; }

int fseek(FILE *f, long off, int whence) {
    fflush(f);
    f->pos = f->len = 0;
    f->ungot = -1;
    f->last_write = 0;
    f->flags &= ~F_EOF;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f) {
    long p = lseek(f->fd, 0, SEEK_CUR);
    if (p < 0) return -1;
    if (f->last_write) return p + (long)f->len;
    return p - (long)(f->len - f->pos) - (f->ungot >= 0 ? 1 : 0);
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }
int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    fflush(f);
    f->mode = mode;
    if (buf && size) { f->buf = buf; f->bufsize = size; }
    return 0;
}

/* ---- printf family ---- */
static void out_file(char c, void *ctx) { fputc(c, (FILE *)ctx); }

/* one recursive lock serialises all stdio output between threads */
#include <pthread.h>
static volatile int stdio_lock;
static void *volatile stdio_owner;
static int stdio_depth;

static void slock(void) {
    void *me = pthread_self();
    if (stdio_owner == me) { stdio_depth++; return; }
    __lock(&stdio_lock);
    stdio_owner = me;
    stdio_depth = 1;
}

static void sunlock(void) {
    if (--stdio_depth == 0) {
        stdio_owner = 0;
        __unlock(&stdio_lock);
    }
}

int fflush(FILE *f) { slock(); int r = fflush_unlocked(f); sunlock(); return r; }
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f) {
    slock();
    size_t r = fwrite_unlocked(buf, size, n, f);
    sunlock();
    return r;
}
int vfprintf(FILE *f, const char *fmt, va_list ap) { slock(); int r = fmt_format(out_file, f, fmt, ap); sunlock(); return r; }
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

typedef struct { char *buf; size_t n, pos; } sbuf_t;
static void out_buf(char c, void *ctx) {
    sbuf_t *s = ctx;
    if (s->pos + 1 < s->n) s->buf[s->pos] = c;
    s->pos++;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    sbuf_t s = { buf, n, 0 };
    fmt_format(out_buf, &s, fmt, ap);
    if (n) buf[s.pos < n ? s.pos : n - 1] = 0;
    return (int)s.pos;
}
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
int sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsprintf(buf, fmt, ap); va_end(ap); return r; }

int dprintf(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(fd, buf, r < (int)sizeof(buf) ? r : (int)sizeof(buf) - 1);
    return r;
}

void perror(const char *msg) {
    if (msg && *msg) fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

int remove(const char *path) {
    if (unlink(path) == 0) return 0;
    if (errno == EISDIR) return rmdir(path);
    return -1;
}

/* ---- minimal sscanf: %d %i %u %x %ld %lu %f %lf %s %c %[...] not supported ---- */
int vsscanf(const char *s, const char *fmt, va_list ap) {
    int count = 0;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) { while (isspace((unsigned char)*s)) s++; fmt++; continue; }
        if (*fmt != '%') { if (*s != *fmt) break; s++; fmt++; continue; }
        fmt++;
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        char c = *fmt++;
        if (c != 'c') while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        char *end;
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x') {
            long v = strtol(s, &end, c == 'x' ? 16 : c == 'i' ? 0 : 10);
            if (end == s) break;
            if (lng) *va_arg(ap, long *) = v; else *va_arg(ap, int *) = (int)v;
            s = end;
        } else if (c == 'f' || c == 'g' || c == 'e') {
            double v = strtod(s, &end);
            if (end == s) break;
            if (lng) *va_arg(ap, double *) = v; else *va_arg(ap, float *) = (float)v;
            s = end;
        } else if (c == 's') {
            char *o = va_arg(ap, char *);
            while (*s && !isspace((unsigned char)*s)) *o++ = *s++;
            *o = 0;
        } else if (c == 'c') {
            *va_arg(ap, char *) = *s++;
        } else break;
        count++;
    }
    return count;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
