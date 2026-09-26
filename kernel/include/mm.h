#pragma once
#include <kernel.h>
#include <boot.h>

/* page table entry flags */
#define PTE_P     (1ULL << 0)
#define PTE_W     (1ULL << 1)
#define PTE_U     (1ULL << 2)
#define PTE_PWT   (1ULL << 3)
#define PTE_PCD   (1ULL << 4)
#define PTE_A     (1ULL << 5)
#define PTE_D     (1ULL << 6)
#define PTE_PS    (1ULL << 7)
#define PTE_G     (1ULL << 8)
#define PTE_OWNED (1ULL << 9)     /* page frame is owned by this mapping (free on unmap) */
#define PTE_NX    (1ULL << 63)
#define PTE_ADDR  0x000FFFFFFFFFF000ULL

#define CACHE_WB 0
#define CACHE_WC PTE_PWT            /* PAT entry 1 = write combining */
#define CACHE_UC (PTE_PCD | PTE_PWT)

/* pmm.c */
void pmm_init(void);
uint64_t pmm_alloc(void);                 /* one page, not zeroed; 0 on failure */
uint64_t pmm_alloc_zero(void);
void pmm_free(uint64_t phys);
uint64_t pmm_alloc_contig(size_t pages, uint64_t max_addr);   /* zeroed */
void pmm_free_contig(uint64_t phys, size_t pages);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_max_phys(void);

/* vmm.c */
extern uint64_t kernel_pml4;
void vmm_init(void);
uint64_t vmm_new_space(void);                       /* returns PML4 phys */
void vmm_free_space(uint64_t pml4);                 /* frees user half */
int vmm_clone_user(uint64_t src, uint64_t dst);      /* copy every user page (fork) */
bool vmm_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags);
uint64_t vmm_unmap(uint64_t pml4, uint64_t va);     /* returns old PTE */
uint64_t vmm_get_pte(uint64_t pml4, uint64_t va);   /* 0 if not mapped */
uint64_t vmm_translate(uint64_t pml4, uint64_t va); /* phys or 0 */
void *ioremap(uint64_t phys, size_t size, uint64_t cache);
void iounmap(void *va);

/* user memory helpers (current address space) */
bool user_range_ok(const void *p, size_t n, bool write);
bool user_str_ok(const char *s, size_t max);
int copy_from_user(void *dst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *src, size_t n);
long strncpy_from_user(char *dst, const char *usrc, size_t n);

/* heap stats */
size_t heap_used_bytes(void);
