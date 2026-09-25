/* Virtual memory: 4-level paging, kernel address space, vmalloc/ioremap, user copies */
#include <kernel.h>
#include <mm.h>
#include <cpu.h>
#include <smp.h>

uint64_t kernel_pml4;

#define PML4_PHYSMAP 256
#define PML4_VMALLOC 384
#define PML4_KERNEL  511

static inline uint64_t *tbl(uint64_t e) { return P2V(e & PTE_ADDR); }

static uint64_t *walk(uint64_t pml4, uint64_t va, bool create) {
    uint64_t *t = tbl(pml4);
    static const int shifts[3] = { 39, 30, 21 };
    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t idx = (va >> shifts[lvl]) & 511;
        uint64_t e = t[idx];
        if (!(e & PTE_P)) {
            if (!create) return 0;
            uint64_t n = pmm_alloc_zero();
            if (!n) return 0;
            t[idx] = n | PTE_P | PTE_W | (va < USER_TOP ? PTE_U : 0);
            e = t[idx];
        } else if (e & PTE_PS) {
            return 0;
        }
        t = tbl(e);
    }
    return &t[(va >> 12) & 511];
}

bool vmm_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t f = irq_save();
    uint64_t *pte = walk(pml4, va, true);
    if (!pte) { irq_restore(f); return false; }
    uint64_t old = *pte;
    *pte = (pa & PTE_ADDR) | flags | PTE_P;
    if (old & PTE_P) tlb_shootdown(va >= USER_TOP ? 0 : pml4, va, 1);
    else invlpg(va);
    irq_restore(f);
    return true;
}

static uint64_t unmap_noflush(uint64_t pml4, uint64_t va) {
    uint64_t *pte = walk(pml4, va, false);
    uint64_t old = 0;
    if (pte) {
        old = *pte;
        *pte = 0;
    }
    return old;
}

uint64_t vmm_unmap(uint64_t pml4, uint64_t va) {
    uint64_t f = irq_save();
    uint64_t old = unmap_noflush(pml4, va);
    if (old & PTE_P) tlb_shootdown(va >= USER_TOP ? 0 : pml4, va, 1);
    irq_restore(f);
    return old;
}

uint64_t vmm_get_pte(uint64_t pml4, uint64_t va) {
    uint64_t *pte = walk(pml4, va, false);
    return pte ? *pte : 0;
}

uint64_t vmm_translate(uint64_t pml4, uint64_t va) {
    uint64_t e = vmm_get_pte(pml4, va);
    if (!(e & PTE_P)) return 0;
    return (e & PTE_ADDR) | (va & 0xFFF);
}

uint64_t vmm_new_space(void) {
    uint64_t p = pmm_alloc_zero();
    if (!p) return 0;
    uint64_t *n = P2V(p), *k = P2V(kernel_pml4);
    for (int i = 256; i < 512; i++) n[i] = k[i];
    return p;
}

void vmm_free_space(uint64_t pml4) {
    uint64_t *l4 = P2V(pml4);
    for (int i = 0; i < 256; i++) {
        if (!(l4[i] & PTE_P)) continue;
        uint64_t *l3 = tbl(l4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(l3[j] & PTE_P)) continue;
            uint64_t *l2 = tbl(l3[j]);
            for (int k = 0; k < 512; k++) {
                if (!(l2[k] & PTE_P)) continue;
                uint64_t *l1 = tbl(l2[k]);
                for (int m = 0; m < 512; m++) {
                    if ((l1[m] & PTE_P) && (l1[m] & PTE_OWNED)) pmm_free(l1[m] & PTE_ADDR);
                }
                pmm_free(l2[k] & PTE_ADDR);
            }
            pmm_free(l3[j] & PTE_ADDR);
        }
        pmm_free(l4[i] & PTE_ADDR);
    }
    pmm_free(pml4);
}

/* ------------------------------------------------------------------ kernel space setup */

static void map_2m(uint64_t *l4, uint64_t va, uint64_t pa, uint64_t flags) {
    uint64_t i4 = (va >> 39) & 511, i3 = (va >> 30) & 511, i2 = (va >> 21) & 511;
    if (!(l4[i4] & PTE_P)) l4[i4] = pmm_alloc_zero() | PTE_P | PTE_W;
    uint64_t *l3 = tbl(l4[i4]);
    if (!(l3[i3] & PTE_P)) l3[i3] = pmm_alloc_zero() | PTE_P | PTE_W;
    uint64_t *l2 = tbl(l3[i3]);
    l2[i2] = (pa & ~0x1FFFFFULL) | flags | PTE_P | PTE_PS;
}

static bool chunk_has_ram(uint64_t start, uint64_t end) {
    if (start < 0x200000) return true;
    for (int i = 0; i < bootinfo.mmap_count; i++) {
        uint32_t t = bootinfo.mmap[i].type;
        if (t != 1 && t != 3 && t != 4) continue;
        uint64_t s = bootinfo.mmap[i].addr, e = s + bootinfo.mmap[i].len;
        if (s < end && e > start) return true;
    }
    return false;
}

void vmm_init(void) {
    kernel_pml4 = pmm_alloc_zero();
    uint64_t *l4 = P2V(kernel_pml4);
    uint64_t nx = cpu_has_nx() ? PTE_NX : 0;

    /* physmap: every 2 MiB chunk that contains RAM */
    uint64_t top = 0;
    for (int i = 0; i < bootinfo.mmap_count; i++) {
        uint32_t t = bootinfo.mmap[i].type;
        if (t != 1 && t != 3 && t != 4) continue;
        uint64_t e = bootinfo.mmap[i].addr + bootinfo.mmap[i].len;
        if (e > top) top = e;
    }
    for (uint64_t pa = 0; pa < top; pa += 0x200000) {
        if (chunk_has_ram(pa, pa + 0x200000))
            map_2m(l4, PHYSMAP_BASE + pa, pa, PTE_W | PTE_G | nx);
    }

    /* kernel image */
    uint64_t kend = ALIGN_UP((uint64_t)_kernel_phys_end, 0x200000);
    for (uint64_t pa = 0; pa < kend; pa += 0x200000)
        map_2m(l4, KERNEL_VMA + pa, pa, PTE_W | PTE_G);

    /* preallocate the vmalloc PDPT so every address space shares it */
    l4[PML4_VMALLOC] = pmm_alloc_zero() | PTE_P | PTE_W;
    /* make sure physmap/kernel slots exist too (they do after the loops) */

    write_cr3(kernel_pml4);
    klog("[vmm] kernel address space ready, physmap up to %lu MiB\n", top >> 20);
}

/* ------------------------------------------------------------------ vmalloc / ioremap */

typedef struct vm_range {
    uint64_t va, pages;
    bool owned;
    struct vm_range *next;
} vm_range_t;

static vm_range_t *free_ranges;    /* sorted by va */
static vm_range_t *used_ranges;
static bool vm_inited;

static void vm_lazy_init(void) {
    if (vm_inited) return;
    vm_inited = true;
    free_ranges = kmalloc(sizeof(vm_range_t));
    free_ranges->va = VMALLOC_BASE;
    free_ranges->pages = VMALLOC_SIZE / PAGE_SIZE;
    free_ranges->next = 0;
}

static uint64_t va_alloc(uint64_t pages) {
    vm_lazy_init();
    for (vm_range_t **pp = &free_ranges; *pp; pp = &(*pp)->next) {
        vm_range_t *r = *pp;
        if (r->pages >= pages) {
            uint64_t va = r->va;
            r->va += pages * PAGE_SIZE;
            r->pages -= pages;
            if (r->pages == 0) {
                *pp = r->next;
                kfree(r);
            }
            return va;
        }
    }
    return 0;
}

static void va_free(uint64_t va, uint64_t pages) {
    vm_range_t **pp = &free_ranges;
    while (*pp && (*pp)->va < va) pp = &(*pp)->next;
    /* merge with next */
    if (*pp && va + pages * PAGE_SIZE == (*pp)->va) {
        (*pp)->va = va;
        (*pp)->pages += pages;
    } else {
        vm_range_t *n = kmalloc(sizeof(vm_range_t));
        n->va = va;
        n->pages = pages;
        n->next = *pp;
        *pp = n;
    }
    /* merge with previous */
    for (vm_range_t *r = free_ranges; r && r->next; r = r->next) {
        if (r->va + r->pages * PAGE_SIZE == r->next->va) {
            vm_range_t *x = r->next;
            r->pages += x->pages;
            r->next = x->next;
            kfree(x);
            break;
        }
    }
}

static void *vm_create(uint64_t pages, uint64_t phys, uint64_t cache, bool owned) {
    uint64_t f = irq_save();
    uint64_t va = va_alloc(pages + 1);   /* +1 guard page */
    if (!va) { irq_restore(f); return 0; }
    vm_range_t *rec = kmalloc(sizeof(vm_range_t));
    rec->va = va;
    rec->pages = pages;
    rec->owned = owned;
    rec->next = used_ranges;
    used_ranges = rec;
    irq_restore(f);

    uint64_t nx = cpu_has_nx() ? PTE_NX : 0;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t pa;
        if (owned) {
            pa = pmm_alloc_zero();
            if (!pa) panic("vmalloc: out of memory");
        } else {
            pa = phys + i * PAGE_SIZE;
        }
        vmm_map(kernel_pml4, va + i * PAGE_SIZE, pa, PTE_W | PTE_G | nx | cache | (owned ? PTE_OWNED : 0));
    }
    return (void *)va;
}

static void vm_destroy(void *p) {
    uint64_t va = (uint64_t)p;
    uint64_t f = irq_save();
    vm_range_t **pp = &used_ranges;
    while (*pp && (*pp)->va != va) pp = &(*pp)->next;
    vm_range_t *rec = *pp;
    if (!rec) { irq_restore(f); klog("[vmm] vfree: bad pointer %p\n", p); return; }
    *pp = rec->next;
    irq_restore(f);
    uint64_t *olds = rec->pages > 1 ? kmalloc(rec->pages * sizeof(uint64_t)) : 0;
    f = irq_save();
    uint64_t one = 0;
    for (uint64_t i = 0; i < rec->pages; i++) {
        uint64_t old = unmap_noflush(kernel_pml4, va + i * PAGE_SIZE);
        if (olds) olds[i] = old; else one = old;
    }
    /* no CPU may still reach the frames before they are handed out again */
    tlb_shootdown(0, va, rec->pages);
    irq_restore(f);
    if (rec->owned) {
        for (uint64_t i = 0; i < rec->pages; i++) {
            uint64_t old = olds ? olds[i] : one;
            if (old & PTE_P) pmm_free(old & PTE_ADDR);
        }
    }
    kfree(olds);
    f = irq_save();
    va_free(va, rec->pages + 1);
    irq_restore(f);
    kfree(rec);
}

void *vmalloc(size_t n) {
    if (!n) return 0;
    return vm_create(PAGE_ALIGN_UP(n) / PAGE_SIZE, 0, CACHE_WB, true);
}

void vfree(void *p) { if (p) vm_destroy(p); }

void *ioremap(uint64_t phys, size_t size, uint64_t cache) {
    uint64_t off = phys & 0xFFF;
    uint64_t pages = PAGE_ALIGN_UP(size + off) / PAGE_SIZE;
    uint8_t *va = vm_create(pages, phys & ~0xFFFULL, cache, false);
    return va ? va + off : 0;
}

void iounmap(void *va) { vm_destroy((void *)PAGE_ALIGN_DOWN((uint64_t)va)); }

/* ------------------------------------------------------------------ user memory */

bool proc_demand_page(uint64_t addr, bool write);

static bool user_page_ok(uint64_t va, bool write) {
    uint64_t pte = vmm_get_pte(read_cr3() & PTE_ADDR, va);
    if ((pte & PTE_P) && (pte & PTE_U) && (!write || (pte & PTE_W))) return true;
    if (!(pte & PTE_P)) return proc_demand_page(va, write);
    return false;
}

bool user_range_ok(const void *p, size_t n, bool write) {
    uint64_t a = (uint64_t)p;
    if (n == 0) return true;
    if (a >= USER_TOP || a + n > USER_TOP || a + n < a) return false;
    for (uint64_t pg = PAGE_ALIGN_DOWN(a); pg < a + n; pg += PAGE_SIZE)
        if (!user_page_ok(pg, write)) return false;
    return true;
}

bool user_str_ok(const char *s, size_t max) {
    uint64_t a = (uint64_t)s;
    for (size_t i = 0; i < max; i++, a++) {
        if (a >= USER_TOP) return false;
        if (i == 0 || (a & 0xFFF) == 0)
            if (!user_page_ok(PAGE_ALIGN_DOWN(a), false)) return false;
        if (*(const char *)a == 0) return true;
    }
    return false;
}

int copy_from_user(void *dst, const void *usrc, size_t n) {
    if (!user_range_ok(usrc, n, false)) return -14;
    memcpy(dst, usrc, n);
    return 0;
}

int copy_to_user(void *udst, const void *src, size_t n) {
    if (!user_range_ok(udst, n, true)) return -14;
    memcpy(udst, src, n);
    return 0;
}

long strncpy_from_user(char *dst, const char *usrc, size_t n) {
    if (!user_str_ok(usrc, n)) return -14;
    size_t len = strnlen(usrc, n);
    if (len >= n) return -36;
    memcpy(dst, usrc, len + 1);
    return (long)len;
}

bool vmm_page_fault(regs_t *r) {
    uint64_t addr = read_cr2();
    if (addr < USER_TOP && !(r->error & 1)) {
        /* not-present fault on a user address: maybe stack growth */
        return proc_demand_page(PAGE_ALIGN_DOWN(addr), (r->error & 2) != 0);
    }
    return false;
}
