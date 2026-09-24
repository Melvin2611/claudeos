/* Temporary weak defaults for subsystems that are not implemented yet. */
#include <kernel.h>

__attribute__((weak)) void sched_tick(void) {}
__attribute__((weak)) void sched_trap_exit(regs_t *r) { UNUSED(r); }
__attribute__((weak)) void syscall_dispatch(regs_t *r) { r->rax = -38; }
__attribute__((weak)) void proc_fault(regs_t *r, const char *what) { panic_regs(r, "user fault: %s", what); }
__attribute__((weak)) bool proc_demand_page(uint64_t addr, bool write) { UNUSED(addr); UNUSED(write); return false; }
