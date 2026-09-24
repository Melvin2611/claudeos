#pragma once
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#define EOF (-1)
#define BUFSIZ 4096
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IONBF 0
#define _IOLBF 1
#define _IOFBF 2

typedef struct FILE {
    int fd;
    int flags;          /* 1 read, 2 write, 4 eof, 8 error */
    int mode;           /* buffering mode */
    char *buf;
    size_t bufsize;
    size_t pos, len;    /* read: pos..len valid; write: len bytes pending */
    int ungot;
    int last_write;     /* last op was a write */
} FILE;

extern FILE *stdin, *stdout, *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int getchar(void);
int ungetc(int c, FILE *f);
char *fgets(char *s, int n, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int putchar(int c);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int fflush(FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
void clearerr(FILE *f);
int fileno(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
long getline(char **line, size_t *cap, FILE *f);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int sscanf(const char *s, const char *fmt, ...);
int vsscanf(const char *s, const char *fmt, va_list ap);
void perror(const char *msg);
int remove(const char *path);
int rename(const char *from, const char *to);
