/* Local APIC, application processor start-up, IPIs, TLB shootdowns and the big kernel lock */
#include <kernel.h>
#include <smp.h>
#include <sched.h>
#include <cpu.h>
#include <mm.h>
#include "drivers.h"

cpu_t cpus[MAX_CPUS];
int cpu_count = 1;
uint32_t lapic_bsp_id;

extern uint32_t acpi_cpu_apic_ids[MAX_CPUS];
extern int acpi_cpu_count;
extern uint64_t acpi_lapic_phys;
bool cmdline_has(const char *opt);
const char *cmdline_get(const char *key);

/* ------------------------------------------------------------------ local APIC access */

#define LAPIC_ID      0x020
#define LAPIC_TPR     0x080
#define LAPIC_EOI     0x0B0
#define LAPIC_SVR     0x0F0
#define LAPIC_ESR     0x280
#define LAPIC_ICR_LO  0x300
#define LAPIC_ICR_HI  0x310
#define LAPIC_LVT_TMR 0x320
#define LAPIC_LINT0   0x350
#define LAPIC_LINT1   0x360
#define LAPIC_LVT_ERR 0x370
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR 0x390
#define LAPIC_TMR_DIV 0x3E0

static volatile uint32_t *lapic;
static bool x2apic;
static uint32_t lapic_ticks_per_ms;

static uint32_t lapic_read(uint32_t reg) {
    if (x2apic) return (uint32_t)rdmsr(0x800 + (reg >> 4));
    return lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t v) {
    if (x2apic) wrmsr(0x800 + (reg >> 4), v);
    else lapic[reg / 4] = v;
}

uint32_t lapic_id(void) {
    if (!lapic && !x2apic) return 0;
    return x2apic ? (uint32_t)rdmsr(0x802) : lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    if (x2apic) wrmsr(0x80B, 0);
    else if (lapic) lapic[LAPIC_EOI / 4] = 0;
}

static void lapic_send_icr(uint32_t dest, uint32_t low) {
    if (x2apic) {
        wrmsr(0x830, ((uint64_t)dest << 32) | low);
        return;
    }
    lapic_write(LAPIC_ICR_HI, dest << 24);
    lapic_write(LAPIC_ICR_LO, low);
    for (int i = 0; i < 1000000 && (lapic_read(LAPIC_ICR_LO) & (1 << 12)); i++) cpu_pause();
}

static void lapic_enable(bool bsp) {
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    /* the boot CPU keeps receiving 8259 interrupts through LINT0 (virtual wire) */
    lapic_write(LAPIC_LINT0, bsp ? 0x700 : 0x10000);
    lapic_write(LAPIC_LINT1, 0x400);                   /* NMI */
    lapic_write(LAPIC_LVT_ERR, 0x10000);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_eoi();
}

static void lapic_timer_start(void) {
    if (!lapic_ticks_per_ms) return;
    lapic_write(LAPIC_TMR_DIV, 0x3);                   /* divide by 16 */
    lapic_write(LAPIC_LVT_TMR, VEC_LAPIC_TIMER | (1 << 17));   /* periodic */
    lapic_write(LAPIC_TMR_INIT, lapic_ticks_per_ms);
}

static void udelay(uint64_t us) {
    uint64_t mhz = cpu_mhz ? cpu_mhz : 2000;
    uint64_t end = rdtsc() + us * mhz;
    while (rdtsc() < end) cpu_pause();
}

void lapic_init_bsp(void) {
    uint64_t base = rdmsr(0x1B);
    x2apic = (base & (1 << 10)) != 0;
    if (!x2apic) {
        uint64_t phys = acpi_lapic_phys ? acpi_lapic_phys : (base & ~0xFFFULL);
        lapic = ioremap(phys, 4096, CACHE_UC);
        wrmsr(0x1B, base | (1 << 11));                 /* global enable */
    }
    lapic_enable(true);
    lapic_bsp_id = lapic_id();
    cpus[0].apic_id = lapic_bsp_id;

    /* calibrate the timer against the PIT tick counter */
    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, 0x10000);               /* masked, one-shot */
    uint64_t t0 = ticks;
    while (ticks == t0) hlt();
    lapic_write(LAPIC_TMR_INIT, 0xFFFFFFFF);
    t0 = ticks;
    while (ticks < t0 + 20) hlt();
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TMR_CUR);
    lapic_write(LAPIC_TMR_INIT, 0);
    lapic_ticks_per_ms = elapsed / 20;
    klog("[smp] local APIC %s id %u, timer %u ticks/ms\n", x2apic ? "(x2APIC)" : "(xAPIC)", lapic_bsp_id,
         lapic_ticks_per_ms);
}

/* ------------------------------------------------------------------ big kernel lock */

static volatile uint32_t bkl_next_ticket, bkl_now_serving;
static volatile int bkl_owner = -1;

void bkl_lock(void) {
    cpu_t *c = this_cpu();
    if (bkl_owner == c->id) { c->bkl_depth++; return; }
    uint32_t t = __atomic_fetch_add(&bkl_next_ticket, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&bkl_now_serving, __ATOMIC_ACQUIRE) != t) smp_spin_poll();
    bkl_owner = c->id;
    c->bkl_depth = 1;
}

void bkl_unlock(void) {
    cpu_t *c = this_cpu();
    if (bkl_owner != c->id) panic("bkl_unlock: not owner (cpu %d, owner %d)", c->id, bkl_owner);
    if (--c->bkl_depth == 0) {
        bkl_owner = -1;
        __atomic_store_n(&bkl_now_serving, bkl_now_serving + 1, __ATOMIC_RELEASE);
    }
}

bool bkl_held(void) { return bkl_owner == this_cpu()->id; }

int bkl_release_all(void) {
    cpu_t *c = this_cpu();
    if (bkl_owner != c->id) return 0;
    int d = c->bkl_depth;
    c->bkl_depth = 1;
    bkl_unlock();
    return d;
}

void bkl_reacquire(int depth) {
    if (depth <= 0) return;
    bkl_lock();
    this_cpu()->bkl_depth = depth;
}

/* ------------------------------------------------------------------ IPIs and TLB shootdown */

static struct {
    volatile uint64_t pml4, va, pages;
    volatile int pending;
} tlb_req;
static spinlock_t tlb_lock = SPINLOCK_INIT;
static volatile bool halting;

void smp_send_ipi(int cpu, uint8_t vector) {
    if (cpu < 0 || cpu >= cpu_count || !cpus[cpu].online) return;
    uint64_t f = irq_save();
    lapic_send_icr(cpus[cpu].apic_id, vector | (1 << 14));
    irq_restore(f);
}

void smp_kick_idle(void) {
    if (cpu_count < 2) return;
    cpu_t *me = this_cpu();
    for (int i = 0; i < cpu_count; i++) {
        cpu_t *c = &cpus[i];
        if (c != me && c->online && c->cur == c->idle && !c->need_resched) {
            c->need_resched = true;
            smp_send_ipi(i, VEC_RESCHED);
            return;
        }
    }
}

static void flush_local(uint64_t pml4, uint64_t va, uint64_t pages) {
    if (pml4 && (read_cr3() & PTE_ADDR) != pml4) return;
    if (pages > 32) {
        if (pml4) {
            write_cr3(read_cr3());
        } else {
            uint64_t cr4 = read_cr4();
            write_cr4(cr4 & ~(1ULL << 7));             /* toggling PGE flushes global pages */
            write_cr4(cr4);
        }
        return;
    }
    for (uint64_t i = 0; i < pages; i++) invlpg(va + i * PAGE_SIZE);
}

void tlb_service(void) {
    cpu_t *c = this_cpu();
    if (!__atomic_exchange_n(&c->tlb_pending, false, __ATOMIC_ACQ_REL)) return;
    flush_local(tlb_req.pml4, tlb_req.va, tlb_req.pages);
    __atomic_fetch_sub(&tlb_req.pending, 1, __ATOMIC_RELEASE);
}

void smp_spin_poll(void) {
    cpu_pause();
    if (halting) { cli(); for (;;) hlt(); }
    cpu_t *c = this_cpu();
    if (c->tlb_pending) tlb_service();
}

void tlb_shootdown(uint64_t pml4, uint64_t va, uint64_t pages) {
    flush_local(pml4, va, pages);
    if (cpu_count < 2) return;
    uint64_t f = irq_save();
    spin_lock(&tlb_lock);
    cpu_t *me = this_cpu();
    tlb_req.pml4 = pml4;
    tlb_req.va = va;
    tlb_req.pages = pages;
    int n = 0;
    bool targets[MAX_CPUS] = { 0 };
    for (int i = 0; i < cpu_count; i++) {
        cpu_t *c = &cpus[i];
        if (c == me || !c->online) continue;
        /* user mappings only matter on CPUs currently running that address space */
        if (pml4 && (!c->cur || c->cur->cr3 != pml4)) continue;
        targets[i] = true;
        n++;
    }
    tlb_req.pending = n;
    for (int i = 0; i < cpu_count; i++) {
        if (!targets[i]) continue;
        __atomic_store_n(&cpus[i].tlb_pending, true, __ATOMIC_RELEASE);
        lapic_send_icr(cpus[i].apic_id, VEC_TLB | (1 << 14));
    }
    while (__atomic_load_n(&tlb_req.pending, __ATOMIC_ACQUIRE) > 0) smp_spin_poll();
    spin_unlock(&tlb_lock);
    irq_restore(f);
}

void smp_halt_others(void) {
    if (cpu_count < 2 || halting) return;
    halting = true;
    uint32_t me = lapic_id();
    for (int i = 0; i < cpu_count; i++)
        if (cpus[i].online && cpus[i].apic_id != me) lapic_send_icr(cpus[i].apic_id, 0x400 | (1 << 14));  /* NMI */
}

bool smp_is_halting(void) { return halting; }

/* ------------------------------------------------------------------ MSI vector table */

static struct { void (*fn)(void *); void *ctx; } msi_handlers[VEC_MSI_COUNT];

bool msi_alloc_vector(void (*fn)(void *), void *ctx, uint8_t *vector_out) {
    for (int i = 0; i < VEC_MSI_COUNT; i++) {
        if (!msi_handlers[i].fn) {
            msi_handlers[i].ctx = ctx;
            msi_handlers[i].fn = fn;
            *vector_out = (uint8_t)(VEC_MSI_BASE + i);
            return true;
        }
    }
    return false;
}

void msi_dispatch(uint8_t vector) {
    int i = vector - VEC_MSI_BASE;
    if (i >= 0 && i < VEC_MSI_COUNT && msi_handlers[i].fn) msi_handlers[i].fn(msi_handlers[i].ctx);
}

/* ------------------------------------------------------------------ application processors */

extern char ap_trampoline_start[], ap_trampoline_end[], ap_trampoline_data[];
#define TRAMPOLINE_PHYS 0x8000

void syscall_cpu_init(void) __attribute__((weak));
void syscall_cpu_init(void) {}

static void ap_entry(cpu_t *c) {
    cpu_setup_percpu(c);
    idt_load();
    cpu_init_features();
    write_cr3(kernel_pml4);
    lapic_enable(false);
    syscall_cpu_init();
    c->cur = c->idle;
    tss_set_rsp0(c->idle->kstack_top);
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    lapic_timer_start();
    sched_idle_loop();
}

void smp_init(void) {
    lapic_init_bsp();
    cpus[0].online = true;
    int want = acpi_cpu_count;
    const char *lim = cmdline_get("maxcpus");
    if (lim && atoi(lim) > 0 && atoi(lim) < want) want = atoi(lim);
    if (cmdline_has("nosmp") || want <= 1) {
        klog("[smp] running on 1 CPU\n");
        return;
    }
    if (kernel_pml4 >= 0x100000000ULL) { klog("[smp] kernel page tables above 4 GiB, no SMP\n"); return; }

    /* trampoline code + temporary identity mapping of the first 2 MiB */
    size_t tsize = ap_trampoline_end - ap_trampoline_start;
    memcpy(P2V(TRAMPOLINE_PHYS), ap_trampoline_start, tsize);
    uint64_t *data = (uint64_t *)P2V(TRAMPOLINE_PHYS + (ap_trampoline_data - ap_trampoline_start));
    uint64_t *l4 = P2V(kernel_pml4);
    uint64_t pdpt = pmm_alloc_zero(), pd = pmm_alloc_zero();
    ((uint64_t *)P2V(pd))[0] = PTE_P | PTE_W | PTE_PS;
    ((uint64_t *)P2V(pdpt))[0] = pd | PTE_P | PTE_W;
    l4[0] = pdpt | PTE_P | PTE_W;
    write_cr3(read_cr3());

    int n = 1;
    for (int i = 0; i < acpi_cpu_count && n < want && n < MAX_CPUS; i++) {
        uint32_t id = acpi_cpu_apic_ids[i];
        if (id == lapic_bsp_id) continue;
        if (!x2apic && id > 254) continue;
        cpu_t *c = &cpus[n];
        memset(c, 0, sizeof(*c));
        c->id = n;
        c->apic_id = id;
        cpu_alloc_percpu(c);
        task_t *idle = sched_create_idle(c);
        data[0] = kernel_pml4;
        data[1] = idle->kstack_top;
        data[2] = (uint64_t)c;
        data[3] = (uint64_t)ap_entry;
        cpu_count = n + 1;            /* lets IPIs reach it once it is online */
        lapic_send_icr(id, 0x4500);                     /* INIT */
        udelay(10000);
        for (int k = 0; k < 2 && !c->online; k++) {
            lapic_send_icr(id, 0x4600 | (TRAMPOLINE_PHYS >> 12));   /* STARTUP */
            udelay(200);
        }
        for (int w = 0; w < 1000 && !__atomic_load_n(&c->online, __ATOMIC_ACQUIRE); w++) udelay(100);
        if (c->online) {
            n++;
        } else {
            klog("[smp] CPU with APIC id %u did not start\n", id);
            cpu_count = n;
        }
    }
    cpu_count = n;
    l4[0] = 0;
    tlb_shootdown(0, 0, 512);
    pmm_free(pd);
    pmm_free(pdpt);
    extern bool lapic_timer_on_bsp;
    lapic_timer_start();          /* the boot CPU also preempts via its APIC timer from now on */
    lapic_timer_on_bsp = true;
    klog("[smp] %d CPU(s) online\n", cpu_count);
}
