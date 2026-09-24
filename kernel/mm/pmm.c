/* Physical memory manager: one bit per 4 KiB frame (1 = used) */
#include <kernel.h>
#include <mm.h>

static uint64_t *bitmap;
static uint64_t max_pfn;
static uint64_t total_usable;
static uint64_t free_count;
static uint64_t search_hint;
static uint64_t max_phys_addr;

typedef struct { uint64_t start, end; } range_t;

static inline void set_used(uint64_t pfn) { bitmap[pfn / 64] |= 1ULL << (pfn % 64); }
static inline void set_free(uint64_t pfn) { bitmap[pfn / 64] &= ~(1ULL << (pfn % 64)); }
static inline bool is_used(uint64_t pfn) { return bitmap[pfn / 64] & (1ULL << (pfn % 64)); }

static void reserve_range(uint64_t start, uint64_t end) {
    start = PAGE_ALIGN_DOWN(start);
    end = PAGE_ALIGN_UP(end);
    for (uint64_t p = start / PAGE_SIZE; p < end / PAGE_SIZE && p < max_pfn; p++) {
        if (!is_used(p)) {
            set_used(p);
            free_count--;
        }
    }
}

void pmm_init(void) {
    bootinfo_t *bi = &bootinfo;
    uint64_t max_addr = 0;
    for (int i = 0; i < bi->mmap_count; i++) {
        if (bi->mmap[i].type != 1) continue;
        uint64_t end = bi->mmap[i].addr + bi->mmap[i].len;
        if (end > max_addr) max_addr = end;
    }
    max_phys_addr = max_addr;
    max_pfn = max_addr / PAGE_SIZE;
    uint64_t bitmap_bytes = ALIGN_UP((max_pfn + 7) / 8, 8);

    /* regions that must not be used for the bitmap */
    range_t reserved[4 + BOOT_MAX_MODULES];
    int nres = 0;
    reserved[nres++] = (range_t){ 0, 0x100000 };
    reserved[nres++] = (range_t){ (uint64_t)_kernel_phys_start, (uint64_t)_kernel_phys_end };
    reserved[nres++] = (range_t){ bi->mbi_phys, bi->mbi_phys + bi->mbi_size };
    for (int i = 0; i < bi->module_count; i++)
        reserved[nres++] = (range_t){ bi->modules[i].start, bi->modules[i].end };

    uint64_t bm_phys = 0;
    for (int i = 0; i < bi->mmap_count && !bm_phys; i++) {
        if (bi->mmap[i].type != 1) continue;
        uint64_t rs = PAGE_ALIGN_UP(bi->mmap[i].addr);
        uint64_t re = bi->mmap[i].addr + bi->mmap[i].len;
        uint64_t cand = rs;
        bool moved = true;
        while (moved) {
            moved = false;
            for (int r = 0; r < nres; r++) {
                if (cand < reserved[r].end && cand + bitmap_bytes > reserved[r].start) {
                    cand = PAGE_ALIGN_UP(reserved[r].end);
                    moved = true;
                }
            }
        }
        if (cand + bitmap_bytes <= re && cand + bitmap_bytes <= 0x100000000ULL) bm_phys = cand;
    }
    if (!bm_phys) panic("pmm: no room for the frame bitmap (%lu bytes)", bitmap_bytes);

    bitmap = P2V(bm_phys);
    memset(bitmap, 0xFF, bitmap_bytes);
    free_count = 0;
    for (int i = 0; i < bi->mmap_count; i++) {
        if (bi->mmap[i].type != 1) continue;
        uint64_t s = PAGE_ALIGN_UP(bi->mmap[i].addr);
        uint64_t e = PAGE_ALIGN_DOWN(bi->mmap[i].addr + bi->mmap[i].len);
        for (uint64_t p = s / PAGE_SIZE; p < e / PAGE_SIZE; p++) {
            if (is_used(p)) {
                set_free(p);
                free_count++;
            }
        }
    }
    total_usable = free_count;
    for (int r = 0; r < nres; r++) reserve_range(reserved[r].start, reserved[r].end);
    reserve_range(bm_phys, bm_phys + bitmap_bytes);
    search_hint = 0x100000 / PAGE_SIZE;

    klog("[pmm] %lu MiB usable, %lu MiB free, bitmap at %lx (%lu KiB)\n",
         total_usable * PAGE_SIZE >> 20, free_count * PAGE_SIZE >> 20, bm_phys, bitmap_bytes >> 10);
}

uint64_t pmm_alloc(void) {
    uint64_t f = irq_save();
    uint64_t words = (max_pfn + 63) / 64;
    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? search_hint / 64 : 0;
        uint64_t stop = pass == 0 ? words : search_hint / 64 + 1;
        for (uint64_t w = start; w < stop && w < words; w++) {
            if (bitmap[w] == ~0ULL) continue;
            uint64_t bit = __builtin_ctzll(~bitmap[w]);
            uint64_t pfn = w * 64 + bit;
            if (pfn >= max_pfn) break;
            set_used(pfn);
            free_count--;
            search_hint = pfn;
            irq_restore(f);
            return pfn * PAGE_SIZE;
        }
    }
    irq_restore(f);
    return 0;
}

uint64_t pmm_alloc_zero(void) {
    uint64_t p = pmm_alloc();
    if (p) memset(P2V(p), 0, PAGE_SIZE);
    return p;
}

void pmm_free(uint64_t phys) {
    uint64_t pfn = phys / PAGE_SIZE;
    if (pfn >= max_pfn) return;
    uint64_t f = irq_save();
    if (!is_used(pfn)) {
        irq_restore(f);
        klog("[pmm] warning: double free of %lx\n", phys);
        return;
    }
    set_free(pfn);
    free_count++;
    if (pfn < search_hint) search_hint = pfn;
    irq_restore(f);
}

uint64_t pmm_alloc_contig(size_t pages, uint64_t max_addr) {
    uint64_t f = irq_save();
    uint64_t limit = MIN(max_pfn, max_addr / PAGE_SIZE);
    uint64_t run = 0, start = 0;
    for (uint64_t p = 0x100000 / PAGE_SIZE; p < limit; p++) {
        if (is_used(p)) { run = 0; continue; }
        if (run == 0) start = p;
        if (++run == pages) {
            for (uint64_t q = start; q < start + pages; q++) set_used(q);
            free_count -= pages;
            irq_restore(f);
            memset(P2V(start * PAGE_SIZE), 0, pages * PAGE_SIZE);
            return start * PAGE_SIZE;
        }
    }
    irq_restore(f);
    return 0;
}

void pmm_free_contig(uint64_t phys, size_t pages) {
    for (size_t i = 0; i < pages; i++) pmm_free(phys + i * PAGE_SIZE);
}

uint64_t pmm_total_pages(void) { return total_usable; }
uint64_t pmm_free_pages(void) { return free_count; }
uint64_t pmm_max_phys(void) { return max_phys_addr; }
