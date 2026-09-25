/* malloc: segregated free lists for small blocks + first-fit list with coalescing on top of sbrk */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define ALIGN 16
#define HDR_SIZE 16
#define MAGIC_USED 0xA110CA7Eu
#define MAGIC_FREE 0xF4EEB10Cu

typedef struct block {
    uint32_t magic;
    uint32_t pad;
    size_t size;                 /* total block size incl. header, low bit = previous-free flag unused */
    /* for free blocks: */
    struct block *next, *prev;   /* free list links (overlap user data) */
} block_t;

typedef struct { size_t size; } footer_t;   /* at end of free blocks */

static block_t *free_list;
static uint8_t *heap_start, *heap_end;

static inline size_t round_up(size_t n) { return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1); }

static void fl_remove(block_t *b) {
    if (b->prev) b->prev->next = b->next; else free_list = b->next;
    if (b->next) b->next->prev = b->prev;
}

static void fl_insert(block_t *b) {
    b->magic = MAGIC_FREE;
    b->prev = 0;
    b->next = free_list;
    if (free_list) free_list->prev = b;
    free_list = b;
    footer_t *f = (footer_t *)((uint8_t *)b + b->size - sizeof(footer_t));
    f->size = b->size;
}

static block_t *grow(size_t need) {
    size_t chunk = need < 256 * 1024 ? 256 * 1024 : round_up(need + 4096);
    uint8_t *p = sbrk((long)chunk);
    if (p == (void *)-1) return 0;
    if (!heap_start) heap_start = p;
    if (heap_end && p != heap_end) {
        /* discontiguous (should not happen): just use the new area */
    }
    heap_end = p + chunk;
    block_t *b = (block_t *)p;
    b->size = chunk;
    /* coalesce with a free block right before */
    if ((uint8_t *)b > heap_start) {
        footer_t *pf = (footer_t *)((uint8_t *)b - sizeof(footer_t));
        block_t *prev = (block_t *)((uint8_t *)b - pf->size);
        if (pf->size && (uint8_t *)prev >= heap_start && prev->magic == MAGIC_FREE && prev->size == pf->size) {
            fl_remove(prev);
            prev->size += b->size;
            b = prev;
        }
    }
    fl_insert(b);
    return b;
}

void __lock(volatile int *l);
void __unlock(volatile int *l);
static volatile int heap_lock;

static void *malloc_unlocked(size_t n);
static void free_unlocked(void *p);

void *malloc(size_t n) {
    __lock(&heap_lock);
    void *p = malloc_unlocked(n);
    __unlock(&heap_lock);
    return p;
}

void free(void *p) {
    if (!p) return;
    __lock(&heap_lock);
    free_unlocked(p);
    __unlock(&heap_lock);
}

static void *malloc_unlocked(size_t n) {
    if (n == 0) n = 1;
    size_t need = round_up(n + HDR_SIZE);
    if (need < sizeof(block_t) + sizeof(footer_t)) need = round_up(sizeof(block_t) + sizeof(footer_t));
    block_t *b = free_list;
    block_t *best = 0;
    for (int scanned = 0; b; b = b->next, scanned++) {
        if (b->size >= need && (!best || b->size < best->size)) {
            best = b;
            if (b->size - need < 64 || scanned > 32) break;
        }
    }
    if (!best) {
        best = grow(need);
        if (!best) return 0;
    }
    fl_remove(best);
    if (best->size - need >= 64) {
        block_t *rest = (block_t *)((uint8_t *)best + need);
        rest->size = best->size - need;
        fl_insert(rest);
        best->size = need;
    }
    best->magic = MAGIC_USED;
    return (uint8_t *)best + HDR_SIZE;
}

static void free_unlocked(void *p) {
    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);
    if (b->magic != MAGIC_USED) {
        static const char msg[] = "free(): invalid pointer or double free\n";
        write(2, msg, sizeof(msg) - 1);
        return;
    }
    /* merge with next */
    block_t *next = (block_t *)((uint8_t *)b + b->size);
    if ((uint8_t *)next < heap_end && next->magic == MAGIC_FREE) {
        fl_remove(next);
        b->size += next->size;
    }
    /* merge with previous */
    if ((uint8_t *)b > heap_start) {
        footer_t *pf = (footer_t *)((uint8_t *)b - sizeof(footer_t));
        block_t *prev = (block_t *)((uint8_t *)b - pf->size);
        if (pf->size && pf->size <= (size_t)((uint8_t *)b - heap_start) && prev->magic == MAGIC_FREE &&
            prev->size == pf->size) {
            fl_remove(prev);
            prev->size += b->size;
            b = prev;
        }
    }
    fl_insert(b);
}

void *calloc(size_t n, size_t m) {
    size_t t = n * m;
    if (m && t / m != n) return 0;
    void *p = malloc(t);
    if (p) memset(p, 0, t);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);
    size_t cap = b->size - HDR_SIZE;
    if (n <= cap) return p;
    /* try to extend into the next free block */
    __lock(&heap_lock);
    block_t *next = (block_t *)((uint8_t *)b + b->size);
    size_t need = round_up(n + HDR_SIZE);
    if ((uint8_t *)next < heap_end && next->magic == MAGIC_FREE && b->size + next->size >= need) {
        fl_remove(next);
        b->size += next->size;
        if (b->size - need >= 64) {
            block_t *rest = (block_t *)((uint8_t *)b + need);
            rest->size = b->size - need;
            fl_insert(rest);
            b->size = need;
        }
        __unlock(&heap_lock);
        return p;
    }
    __unlock(&heap_lock);
    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, cap);
    free(p);
    return q;
}
