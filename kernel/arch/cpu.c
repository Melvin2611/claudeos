/* GDT, TSS, IDT and CPU feature setup */
#include <kernel.h>
#include <cpu.h>
#include <smp.h>

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3, iomap_base;
} PACKED tss_t;

typedef struct { uint16_t limit; uint64_t base; } PACKED dtr_t;

typedef struct {
    uint16_t off_lo, sel;
    uint8_t ist, flags;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} PACKED idt_entry_t;

/* one GDT for all CPUs: 5 segment descriptors + a 16-byte TSS descriptor per CPU */
static uint64_t gdt[5 + 2 * MAX_CPUS];
static tss_t tss_bsp;
static idt_entry_t idt[256];
static uint8_t df_stack[16384] __attribute__((aligned(16)));
static uint8_t nmi_stack[8192] __attribute__((aligned(16)));

uint8_t fpu_initial_state[512] __attribute__((aligned(16)));
char cpu_vendor[13];
uint64_t cpu_mhz;
static bool has_nx;

extern void gdt_flush(dtr_t *gdtr);
extern uint64_t isr_stub_table[256];

static void gdt_set_tss(int cpu, tss_t *tss) {
    uint64_t base = (uint64_t)tss;
    uint64_t limit = sizeof(tss_t) - 1;
    gdt[5 + 2 * cpu] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
                       (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6 + 2 * cpu] = base >> 32;
}

/* allocate TSS and IST stacks for an application processor (runs on the BSP) */
void cpu_alloc_percpu(cpu_t *c) {
    if (!c->tss) {
        tss_t *tss = kzalloc(sizeof(tss_t));
        c->ist_df = kmalloc(16384);
        c->ist_nmi = kmalloc(8192);
        tss->ist[0] = (uint64_t)c->ist_df + 16384;
        tss->ist[1] = (uint64_t)c->ist_nmi + 8192;
        tss->iomap_base = sizeof(tss_t);
        c->tss = tss;
    }
}

/* load the shared GDT, this CPU's TSS and the GS base pointing at its cpu_t */
void cpu_setup_percpu(cpu_t *c) {
    tss_t *tss = c->tss;
    gdt_set_tss(c->id, tss);
    dtr_t gdtr = { sizeof(gdt) - 1, (uint64_t)gdt };
    gdt_flush(&gdtr);
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)(TSS_SEL + 16 * c->id)));
    c->self = c;
    wrmsr(0xC0000101, (uint64_t)c);   /* GS base */
    wrmsr(0xC0000102, 0);             /* kernel GS base = user GS while in kernel */
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kernel code */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* 0x18 user data (DPL3) */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* 0x20 user code (DPL3) */

    memset(&tss_bsp, 0, sizeof(tss_bsp));
    tss_bsp.ist[0] = (uint64_t)(df_stack + sizeof(df_stack));
    tss_bsp.ist[1] = (uint64_t)(nmi_stack + sizeof(nmi_stack));
    tss_bsp.iomap_base = sizeof(tss_bsp);
    cpus[0].id = 0;
    cpus[0].tss = &tss_bsp;
    cpu_setup_percpu(&cpus[0]);
}

void tss_set_rsp0(uint64_t rsp0) {
    cpu_t *c = this_cpu();
    ((tss_t *)c->tss)->rsp0 = rsp0;
    c->kernel_rsp = rsp0;
}

static void idt_set(int n, uint64_t handler, uint8_t flags, uint8_t ist) {
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].sel = KERNEL_CS;
    idt[n].ist = ist;
    idt[n].flags = flags;
    idt[n].off_mid = (handler >> 16) & 0xFFFF;
    idt[n].off_hi = handler >> 32;
    idt[n].zero = 0;
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) idt_set(i, isr_stub_table[i], 0x8E, 0);   /* interrupt gate */
    idt_set(8, isr_stub_table[8], 0x8E, 1);      /* double fault on IST1 */
    idt_set(2, isr_stub_table[2], 0x8E, 2);      /* NMI on IST2 */
    idt_set(0x80, isr_stub_table[0x80], 0xEE, 0); /* syscall gate, DPL3 */
    idt_load();
}

void idt_load(void) {
    dtr_t idtr = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
}

void cpu_init_features(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    memcpy(cpu_vendor, &b, 4);
    memcpy(cpu_vendor + 4, &d, 4);
    memcpy(cpu_vendor + 8, &c, 4);
    cpu_vendor[12] = 0;

    /* SSE: CR0.EM=0, CR0.MP=1, CR0.NE=1; CR4.OSFXSR, CR4.OSXMMEXCPT */
    uint64_t cr0 = read_cr0();
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1) | (1ULL << 5);
    cr0 &= ~(1ULL << 3);          /* TS */
    write_cr0(cr0);
    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 9) | (1ULL << 10);
    cpuid(1, 0, &a, &b, &c, &d);
    if (d & (1u << 13)) cr4 |= (1ULL << 7);   /* PGE */
    write_cr4(cr4);

    /* NX */
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    if (d & (1u << 20)) {
        wrmsr(0xC0000080, rdmsr(0xC0000080) | (1ULL << 11));
        has_nx = true;
    }

    /* PAT: entry 1 (PWT=1) = write combining, keep others default (WB, WT, UC-, UC) */
    cpuid(1, 0, &a, &b, &c, &d);
    if (d & (1u << 16)) {
        uint64_t pat = 0x0007040600070106ULL;   /* PA0 WB, PA1 WC, PA2 UC-, PA3 UC, PA4 WB, PA5 WC ... */
        wrmsr(0x277, pat);
    }

    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    static bool saved;
    if (!saved) { fxsave(fpu_initial_state); saved = true; }
}

bool cpu_has_nx(void) { return has_nx; }

void cpu_get_brand(char *out, size_t n) {
    uint32_t regs[12];
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) { strlcpy(out, cpu_vendor, n); return; }
    for (int i = 0; i < 3; i++)
        cpuid(0x80000002 + i, 0, &regs[i * 4], &regs[i * 4 + 1], &regs[i * 4 + 2], &regs[i * 4 + 3]);
    char brand[49];
    memcpy(brand, regs, 48);
    brand[48] = 0;
    char *s = brand;
    while (*s == ' ') s++;
    strlcpy(out, s, n);
    size_t l = strlen(out);
    while (l && out[l - 1] == ' ') out[--l] = 0;
    /* collapse runs of spaces */
    char *w = out;
    for (char *r = out; *r; r++)
        if (!(r[0] == ' ' && r[1] == ' ')) *w++ = *r;
    *w = 0;
}

/* estimate the TSC frequency using the PIT tick counter (call with interrupts enabled) */
void cpu_measure_mhz(void) {
    extern volatile uint64_t ticks;
    uint64_t t0 = ticks;
    while (ticks == t0) hlt();
    uint64_t c0 = rdtsc();
    t0 = ticks;
    while (ticks < t0 + 50) hlt();
    uint64_t c1 = rdtsc();
    cpu_mhz = (c1 - c0) / 50 / 1000;
}
