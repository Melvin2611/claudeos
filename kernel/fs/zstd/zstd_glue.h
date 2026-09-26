#pragma once
/* Minimal C library environment for the vendored zstd reference decoder */
#include <stdint.h>
#include <stddef.h>

#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"

void *kmalloc(size_t n);
void kfree(void *p);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);

void *kzalloc(size_t n);
#define malloc(n) kmalloc(n)
#define calloc(n, m) kzalloc((n) * (m))
#define free(p) kfree(p)

/* does not return: jumps back into zstd_decompress() */
__attribute__((noreturn)) void zstd_error(const char *msg);
