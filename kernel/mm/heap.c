/* Kernel heap: power-of-two slab classes (16..2048 bytes incl. header) carved from
 * physmap pages; larger objects come from vmalloc. Every object has a 16-byte header. */
#include <kernel.h>
#include <mm.h>

#define HEAP_MAGIC 0xC1A0DE05u
#define NCLASSES 8
#define LARGE_CLASS 0xFF

typedef struct {
    uint32_t magic;
    uint32_t cls;
    uint64_t size;        /* requested size */
} hdr_t;

typedef struct free_obj { struct free_obj *next; } free_obj_t;

static free_obj_t *freelist[NCLASSES];
static size_t used_bytes;

static bool refill(int cls) {
    uint64_t pa = pmm_alloc();
    if (!pa) return false;
    uint8_t *page = P2V(pa);
    size_t osz = 16u << cls;
    for (size_t off = 0; off + osz <= PAGE_SIZE; off += osz) {
        free_obj_t *o = (free_obj_t *)(page + off);
        o->next = freelist[cls];
        freelist[cls] = o;
    }
    return true;
}

void *kmalloc(size_t n) {
    size_t total = n + sizeof(hdr_t);
    if (total <= 2048) {
        int cls = 0;
        while ((16u << cls) < total) cls++;
        uint64_t f = irq_save();
        if (!freelist[cls] && !refill(cls)) { irq_restore(f); return 0; }
        free_obj_t *o = freelist[cls];
        freelist[cls] = o->next;
        used_bytes += 16u << cls;
        irq_restore(f);
        hdr_t *h = (hdr_t *)o;
        h->magic = HEAP_MAGIC;
        h->cls = cls;
        h->size = n;
        return h + 1;
    }
    hdr_t *h = vmalloc(total);
    if (!h) return 0;
    h->magic = HEAP_MAGIC;
    h->cls = LARGE_CLASS;
    h->size = n;
    uint64_t f = irq_save();
    used_bytes += PAGE_ALIGN_UP(total);
    irq_restore(f);
    return h + 1;
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    hdr_t *h = (hdr_t *)p - 1;
    if (h->magic != HEAP_MAGIC) panic("kfree: bad pointer %p (magic %x)", p, h->magic);
    h->magic = 0xDEADBEEF;
    if (h->cls == LARGE_CLASS) {
        uint64_t f = irq_save();
        used_bytes -= PAGE_ALIGN_UP(h->size + sizeof(hdr_t));
        irq_restore(f);
        vfree(h);
        return;
    }
    int cls = h->cls;
    uint64_t f = irq_save();
    free_obj_t *o = (free_obj_t *)h;
    o->next = freelist[cls];
    freelist[cls] = o;
    used_bytes -= 16u << cls;
    irq_restore(f);
}

void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    if (!n) { kfree(p); return 0; }
    hdr_t *h = (hdr_t *)p - 1;
    if (h->magic != HEAP_MAGIC) panic("krealloc: bad pointer %p", p);
    size_t cap = h->cls == LARGE_CLASS ? PAGE_ALIGN_UP(h->size + sizeof(hdr_t)) - sizeof(hdr_t)
                                       : (16u << h->cls) - sizeof(hdr_t);
    if (n <= cap) { h->size = n; return p; }
    void *q = kmalloc(n);
    if (!q) return 0;
    memcpy(q, p, h->size);
    kfree(p);
    return q;
}

size_t heap_used_bytes(void) { return used_bytes; }
