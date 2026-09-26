#pragma once
#include <kernel.h>

typedef void (*irq_handler_t)(regs_t *r, void *ctx);

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);
void idt_init(void);
void idt_load(void);
void cpu_init_features(void);
bool cpu_has_nx(void);
void cpu_get_brand(char *out, size_t n);
extern char cpu_vendor[13];
extern uint64_t cpu_mhz;
void cpu_measure_mhz(void);

void pic_init(void);
void irq_register(int irq, irq_handler_t h, void *ctx);
void irq_unmask(int irq);
void irq_mask(int irq);

void pit_init(uint32_t hz);

/* fxsave/fxrstor helpers (area must be 16-byte aligned, 512 bytes) */
static inline void fxsave(void *area) { __asm__ volatile("fxsave64 (%0)" :: "r"(area) : "memory"); }
static inline void fxrstor(void *area) { __asm__ volatile("fxrstor64 (%0)" :: "r"(area) : "memory"); }
extern uint8_t fpu_initial_state[4096];
extern uint32_t fpu_size;          /* bytes of per-task FPU/SSE/AVX state */
extern bool fpu_xsave;
/* save/restore the full extended state (XSAVE when available; area 64-byte aligned) */
static inline void fpu_save(void *area) {
    if (fpu_xsave) __asm__ volatile("xsave64 (%0)" :: "r"(area), "a"(-1), "d"(-1) : "memory");
    else fxsave(area);
}
static inline void fpu_restore(void *area) {
    if (fpu_xsave) __asm__ volatile("xrstor64 (%0)" :: "r"(area), "a"(-1), "d"(-1) : "memory");
    else fxrstor(area);
}

static inline bool regs_from_user(regs_t *r) { return (r->cs & 3) == 3; }
