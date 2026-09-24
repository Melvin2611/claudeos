#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "arch.h"

#define KERNEL_VMA   0xFFFFFFFF80000000ULL
#define PHYSMAP_BASE 0xFFFF800000000000ULL
#define VMALLOC_BASE 0xFFFFC00000000000ULL
#define VMALLOC_SIZE 0x0000008000000000ULL   /* 512 GiB */

#define USER_TOP        0x0000800000000000ULL
#define USER_STACK_TOP  0x00007FFFFFFFF000ULL
#define USER_STACK_MAX  (8ULL << 20)
#define USER_LOAD_MIN   0x0000000000400000ULL

#define PAGE_SIZE 4096UL
#define PAGE_ALIGN_UP(x)   (((uint64_t)(x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((uint64_t)(x) & ~(PAGE_SIZE - 1))

#define P2V(p) ((void *)((uint64_t)(p) + PHYSMAP_BASE))
#define V2P(v) ((uint64_t)(v) - PHYSMAP_BASE)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define UNUSED(x) ((void)(x))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define PACKED __attribute__((packed))
#define NORETURN __attribute__((noreturn))

extern char _kernel_start[], _kernel_end[], _kernel_phys_start[], _kernel_phys_end[];

/* lib/string.c */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int atoi(const char *s);
int toupper(int c);
int tolower(int c);
int isdigit(int c);
int isspace(int c);
int isalpha(int c);
int isalnum(int c);

/* lib/printf.c */
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* core/log.c */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
#define klog(...) kprintf(__VA_ARGS__)
size_t klog_read(char *buf, size_t off, size_t n);
size_t klog_size(void);

/* core/panic.c */
NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
NORETURN void panic_regs(regs_t *r, const char *fmt, ...);
#define assert(x) do { if (!(x)) panic("assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__); } while (0)

/* kernel heap (mm/heap.c) */
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
void *vmalloc(size_t n);       /* page-aligned, zeroed, virtually contiguous */
void vfree(void *p);

/* time (core/time.c) */
extern volatile uint64_t ticks;   /* milliseconds since boot */
uint64_t uptime_ms(void);
uint64_t time_now(void);          /* unix epoch seconds */
