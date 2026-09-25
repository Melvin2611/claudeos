#pragma once
/* Multiprocessor support: per-CPU data, spinlocks, the big kernel lock, IPIs */
#include <kernel.h>

#define MAX_CPUS 32

/* interrupt vectors used by the local APIC */
#define VEC_LAPIC_TIMER 0xF0
#define VEC_TLB         0xF1
#define VEC_RESCHED     0xF2
#define VEC_HALT        0xF3
#define VEC_MSI_BASE    0xA0      /* 0xA0..0xDF: MSI/MSI-X vectors for drivers */
#define VEC_MSI_COUNT   64
#define VEC_SPURIOUS    0xFF

struct task;

typedef struct cpu {
    struct cpu *self;            /* %gs:0 */
    struct task *cur;            /* %gs:8  (read through the "current" macro) */
    uint64_t user_rsp;           /* %gs:16  scratch for the syscall instruction */
    uint64_t kernel_rsp;         /* %gs:24  kernel stack top of the current task */
    int id;
    uint32_t apic_id;
    struct task *idle;
    struct task *prev;           /* task we just switched away from */
    volatile bool need_resched;
    volatile bool online;
    volatile bool tlb_pending;
    int slice;
    int bkl_depth;
    uint64_t busy_ms, idle_ms, busy_snap, idle_snap;
    int load;                    /* percent over the last second */
    void *tss;
    uint8_t *ist_df, *ist_nmi;
} cpu_t;

extern cpu_t cpus[MAX_CPUS];
extern int cpu_count;            /* CPUs online */

static inline cpu_t *this_cpu(void) {
    cpu_t *c;
    __asm__("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* the running task never changes under our feet, so the read may be cached */
static inline struct task *get_current(void) {
    struct task *t;
    __asm__("mov %%gs:8, %0" : "=r"(t));
    return t;
}

/* ------------------------------------------------------------------ spinlocks */

void smp_spin_poll(void);        /* called inside every spin loop: serves TLB shootdowns */

typedef struct { volatile uint32_t locked; } spinlock_t;
#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) smp_spin_poll();
}
static inline bool spin_trylock(spinlock_t *l) { return !__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE); }
static inline void spin_unlock(spinlock_t *l) { __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE); }
static inline uint64_t spin_lock_irqsave(spinlock_t *l) { uint64_t f = irq_save(); spin_lock(l); return f; }
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { spin_unlock(l); irq_restore(f); }

/* ------------------------------------------------------------------ big kernel lock
 * Kernel code runs holding the BKL (taken on every entry from user mode or idle,
 * dropped when returning to user mode or switching tasks). User code runs in
 * parallel on all CPUs. Only call these with interrupts disabled. */
void bkl_lock(void);
void bkl_unlock(void);
int bkl_release_all(void);       /* returns the depth that was held */
void bkl_reacquire(int depth);
bool bkl_held(void);

/* ------------------------------------------------------------------ APIC / IPIs */
void lapic_init_bsp(void);
void lapic_eoi(void);
uint32_t lapic_id(void);
void smp_init(void);             /* starts the application processors */
void smp_send_ipi(int cpu, uint8_t vector);
void smp_kick_idle(void);        /* wake an idle CPU to pick up new work */
void smp_halt_others(void);      /* panic: stop every other CPU */
void tlb_shootdown(uint64_t pml4, uint64_t va, uint64_t pages);   /* pml4 0 = kernel mapping */
void tlb_service(void);
void cpu_setup_percpu(cpu_t *c); /* GDT/TSS/GS base for the calling CPU */
void cpu_alloc_percpu(cpu_t *c);
bool msi_alloc_vector(void (*fn)(void *), void *ctx, uint8_t *vector_out);
void msi_dispatch(uint8_t vector);
extern uint32_t lapic_bsp_id;
