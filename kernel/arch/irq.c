/* 8259 PIC, IRQ handler registry and the central interrupt dispatcher */
#include <kernel.h>
#include <cpu.h>
#include <smp.h>

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define MAX_SHARED 4
static struct { irq_handler_t fn; void *ctx; } handlers[16][MAX_SHARED];
static uint16_t irq_mask_bits = 0xFFFF;
uint64_t irq_counts[16];

static const char *exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range exceeded",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor segment overrun",
    "Invalid TSS", "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 floating-point exception", "Alignment check", "Machine check",
    "SIMD floating-point exception", "Virtualization exception", "Control protection exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor injection",
    "VMM communication", "Security exception", "Reserved"
};

const char *exception_name(int v) { return v < 32 ? exc_names[v] : "Unknown"; }

static void pic_write_mask(void) {
    outb(PIC1_DATA, irq_mask_bits & 0xFF);
    outb(PIC2_DATA, irq_mask_bits >> 8);
}

void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();   /* vector offset 32 */
    outb(PIC2_DATA, 0x28); io_wait();   /* vector offset 40 */
    outb(PIC1_DATA, 4); io_wait();
    outb(PIC2_DATA, 2); io_wait();
    outb(PIC1_DATA, 1); io_wait();
    outb(PIC2_DATA, 1); io_wait();
    irq_mask_bits = 0xFFFF & ~(1 << 2);   /* cascade line open */
    pic_write_mask();
}

void irq_unmask(int irq) {
    uint64_t f = irq_save();
    irq_mask_bits &= ~(1 << irq);
    pic_write_mask();
    irq_restore(f);
}

void irq_mask(int irq) {
    uint64_t f = irq_save();
    irq_mask_bits |= (1 << irq);
    pic_write_mask();
    irq_restore(f);
}

void irq_register(int irq, irq_handler_t h, void *ctx) {
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_SHARED; i++) {
        if (!handlers[irq][i].fn) {
            handlers[irq][i].fn = h;
            handlers[irq][i].ctx = ctx;
            break;
        }
    }
    irq_restore(f);
    irq_unmask(irq);
}

static void pic_eoi(int irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* hooks implemented by other subsystems */
void syscall_dispatch(regs_t *r);
bool vmm_page_fault(regs_t *r);
void proc_fault(regs_t *r, const char *what);
void sched_trap_exit(regs_t *r);

bool smp_is_halting(void);
void pit_tick_update(void);
void sched_tick_local(void);

void isr_dispatch(regs_t *r) {
    uint64_t v = r->vector;
    /* vectors that must work without the big kernel lock */
    if (v == VEC_TLB) { tlb_service(); lapic_eoi(); return; }
    if (v == VEC_SPURIOUS) return;
    if (v == 2 && smp_is_halting()) { cli(); for (;;) hlt(); }
    if (v == 32) pit_tick_update();              /* keep time even while waiting for the lock */

    bkl_lock();
    if (v < 32) {
        if (v == 14 && vmm_page_fault(r)) {
            /* resolved (demand paging) */
        } else if (regs_from_user(r)) {
            proc_fault(r, exc_names[v]);
        } else {
            panic_regs(r, "%s in kernel mode", exc_names[v]);
        }
    } else if (v >= 32 && v < 48) {
        int irq = v - 32;
        /* spurious IRQ 7 / 15 check */
        if (irq == 7 || irq == 15) {
            outb(irq == 7 ? PIC1_CMD : PIC2_CMD, 0x0B);
            uint8_t isr = inb(irq == 7 ? PIC1_CMD : PIC2_CMD);
            if (!(isr & 0x80)) {
                if (irq == 15) outb(PIC1_CMD, 0x20);
                bkl_unlock();
                return;
            }
        }
        irq_counts[irq]++;
        pic_eoi(irq);
        for (int i = 0; i < MAX_SHARED; i++)
            if (handlers[irq][i].fn) handlers[irq][i].fn(r, handlers[irq][i].ctx);
    } else if (v == 0x80) {
        sti();
        syscall_dispatch(r);
        cli();
    } else if (v == VEC_LAPIC_TIMER) {
        lapic_eoi();
        sched_tick_local();
    } else if (v == VEC_RESCHED) {
        lapic_eoi();
        this_cpu()->need_resched = true;
    } else if (v >= VEC_MSI_BASE && v < VEC_MSI_BASE + VEC_MSI_COUNT) {
        lapic_eoi();
        msi_dispatch((uint8_t)v);
    }
    sched_trap_exit(r);
    bkl_unlock();
}
