/* Kernel panic: dump to serial and paint a panic screen on the framebuffer. */
#include <kernel.h>
#include <cpu.h>

const char *exception_name(int v);
void panic_screen(const char *msg, regs_t *r);   /* drawn by the framebuffer console */

static char panic_msg[512];
static volatile bool in_panic;

static void backtrace(uint64_t rbp) {
    kprintf("backtrace:\n");
    for (int i = 0; i < 16 && rbp >= KERNEL_VMA && (rbp & 7) == 0; i++) {
        uint64_t *frame = (uint64_t *)rbp;
        uint64_t ret = frame[1];
        if (ret < KERNEL_VMA) break;
        kprintf("  #%d %lx\n", i, ret);
        rbp = frame[0];
    }
}

void smp_halt_others(void);

static NORETURN void do_panic(regs_t *r) {
    smp_halt_others();
    kprintf("\n*** KERNEL PANIC: %s\n", panic_msg);
    if (r) {
        kprintf("RIP=%lx CS=%lx RFLAGS=%lx RSP=%lx SS=%lx ERR=%lx CR2=%lx\n", r->rip, r->cs, r->rflags,
                r->rsp, r->ss, r->error, read_cr2());
        kprintf("RAX=%lx RBX=%lx RCX=%lx RDX=%lx RSI=%lx RDI=%lx RBP=%lx\n", r->rax, r->rbx, r->rcx,
                r->rdx, r->rsi, r->rdi, r->rbp);
        kprintf("R8=%lx R9=%lx R10=%lx R11=%lx R12=%lx R13=%lx R14=%lx R15=%lx\n", r->r8, r->r9, r->r10,
                r->r11, r->r12, r->r13, r->r14, r->r15);
        backtrace(r->rbp);
    } else {
        uint64_t rbp;
        __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
        backtrace(rbp);
    }
    panic_screen(panic_msg, r);
    for (;;) { cli(); hlt(); }
}

NORETURN void panic(const char *fmt, ...) {
    cli();
    if (in_panic) for (;;) hlt();
    in_panic = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(panic_msg, sizeof(panic_msg), fmt, ap);
    va_end(ap);
    do_panic(0);
}

NORETURN void panic_regs(regs_t *r, const char *fmt, ...) {
    cli();
    if (in_panic) for (;;) hlt();
    in_panic = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(panic_msg, sizeof(panic_msg), fmt, ap);
    va_end(ap);
    do_panic(r);
}

__attribute__((weak)) void panic_screen(const char *msg, regs_t *r) { UNUSED(msg); UNUSED(r); }
