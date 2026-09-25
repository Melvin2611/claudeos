/* Preemptive round-robin SMP scheduler, kernel threads, wait queues, mutexes.
 *
 * All CPUs share one run queue protected by sched_lock. The lock is held across
 * the context switch and released by the task that is switched to, so a task can
 * never be picked up by another CPU while its stack is still in use. Everything
 * else (sleep list, wait queues, task list) is protected by the big kernel lock. */
#include <kernel.h>
#include <sched.h>
#include <cpu.h>
#include <mm.h>

waitq_t poll_wq;

static spinlock_t sched_lock = SPINLOCK_INIT;
static task_t *rq_head, *rq_tail;
static task_t *rq_hi_head, *rq_hi_tail;
static task_t *sleepers;
static task_t *all_tasks;
static task_t *reap_list;
static int next_pid = 1;
#define QUANTUM_MS 10

extern void swtch(uint64_t *save, uint64_t new_rsp);
extern void kthread_trampoline(void);

static void rq_push(task_t *t) {
    t->rq_next = 0;
    if (t->prio > 0) {
        if (rq_hi_tail) rq_hi_tail->rq_next = t; else rq_hi_head = t;
        rq_hi_tail = t;
    } else {
        if (rq_tail) rq_tail->rq_next = t; else rq_head = t;
        rq_tail = t;
    }
}

static task_t *rq_pop(void) {
    task_t *t = rq_hi_head;
    if (t) {
        rq_hi_head = t->rq_next;
        if (!rq_hi_head) rq_hi_tail = 0;
        return t;
    }
    t = rq_head;
    if (t) {
        rq_head = t->rq_next;
        if (!rq_head) rq_tail = 0;
    }
    return t;
}

static void sleep_list_remove(task_t *t) {
    if (!t->on_sleep_list) return;
    for (task_t **pp = &sleepers; *pp; pp = &(*pp)->sleep_next) {
        if (*pp == t) { *pp = t->sleep_next; break; }
    }
    t->on_sleep_list = false;
}

static void wq_remove(task_t *t) {
    if (!t->wq) return;
    for (task_t **pp = &t->wq->head; *pp; pp = &(*pp)->wq_next) {
        if (*pp == t) { *pp = t->wq_next; break; }
    }
    t->wq = 0;
}

/* make a blocked/sleeping task runnable (interrupts disabled, BKL held) */
static void make_ready(task_t *t) {
    if (t->state != T_BLOCKED && t->state != T_SLEEPING) return;
    sleep_list_remove(t);
    wq_remove(t);
    spin_lock(&sched_lock);
    t->state = T_READY;
    rq_push(t);
    spin_unlock(&sched_lock);
    cpu_t *c = this_cpu();
    if (t->prio > c->cur->prio || c->cur == c->idle) c->need_resched = true;
    else smp_kick_idle();
}

void task_wake(task_t *t) {
    uint64_t f = irq_save();
    make_ready(t);
    irq_restore(f);
}

task_t *task_alloc(const char *name) {
    task_t *t = kzalloc(sizeof(task_t));
    if (!t) return 0;
    t->kstack = vmalloc(KSTACK_SIZE);
    if (!t->kstack) { kfree(t); return 0; }
    t->kstack_top = (uint64_t)t->kstack + KSTACK_SIZE;
    t->fpu_alloc = kmalloc(512 + 16);
    t->fpu = (uint8_t *)ALIGN_UP((uint64_t)t->fpu_alloc, 16);
    memcpy(t->fpu, fpu_initial_state, 512);
    strlcpy(t->name, name, sizeof(t->name));
    t->cr3 = kernel_pml4;
    t->start_ms = uptime_ms();
    strlcpy(t->cwd, "/", sizeof(t->cwd));
    t->leader = t;
    t->nthreads = 1;
    uint64_t f = irq_save();
    t->pid = next_pid++;
    t->all_next = all_tasks;
    all_tasks = t;
    irq_restore(f);
    return t;
}

void task_free(task_t *t) {
    uint64_t f = irq_save();
    for (task_t **pp = &all_tasks; *pp; pp = &(*pp)->all_next) {
        if (*pp == t) { *pp = t->all_next; break; }
    }
    irq_restore(f);
    if (t->kstack) vfree(t->kstack);
    kfree(t->fpu_alloc);
    kfree(t);
}

task_t *task_find(int pid) {
    for (task_t *t = all_tasks; t; t = t->all_next)
        if (t->pid == pid && t->state != T_DEAD) return t;
    return 0;
}

task_t *task_list(void) { return all_tasks; }

void sched_add(task_t *t) {
    uint64_t f = spin_lock_irqsave(&sched_lock);
    t->state = T_READY;
    rq_push(t);
    spin_unlock_irqrestore(&sched_lock, f);
    smp_kick_idle();
}

task_t *kthread_create(const char *name, int (*fn)(void *), void *arg) {
    task_t *t = task_alloc(name);
    if (!t) return 0;
    uint64_t *sp = (uint64_t *)t->kstack_top;
    *--sp = 0;                              /* alignment / fake return */
    *--sp = (uint64_t)kthread_trampoline;   /* swtch returns here */
    *--sp = 0;                              /* rbp */
    *--sp = 0;                              /* rbx */
    *--sp = (uint64_t)fn;                   /* r12 */
    *--sp = (uint64_t)arg;                  /* r13 */
    *--sp = 0;                              /* r14 */
    *--sp = 0;                              /* r15 */
    t->rsp = (uint64_t)sp;
    t->bkl_depth = 1;                       /* kernel threads run holding the BKL */
    sched_add(t);
    return t;
}

/* free resources of tasks that died (BKL held, never the current task) */
void proc_release_resources(task_t *t);
static void reap(void) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&sched_lock);
        task_t *t = reap_list;
        if (t) reap_list = t->rq_next;
        spin_unlock_irqrestore(&sched_lock, f);
        if (!t) break;
        if (t->is_user && t->leader != t) {
            /* a thread of a still existing process */
            task_t *leader = t->leader;
            task_free(t);
            leader->nthreads--;
            wq_wake_all(&leader->thread_wq);
        } else if (t->is_user) {
            proc_release_resources(t);
            if (t->state == T_DEAD || t->waited) {
                task_free(t);
            } else {
                /* the task struct stays as zombie until waited for */
                vfree(t->kstack);
                t->kstack = 0;
            }
        } else {
            task_free(t);
        }
    }
}

/* first half of the switch epilogue: runs on the new task with sched_lock held */
static void switch_finish(void) {
    cpu_t *c = this_cpu();
    task_t *prev = c->prev;
    c->prev = 0;
    if (prev && (prev->state == T_ZOMBIE || prev->state == T_DEAD)) {
        /* its stack is no longer in use: hand it to the reaper */
        prev->rq_next = reap_list;
        reap_list = prev;
    }
    spin_unlock(&sched_lock);
}

/* called by new tasks (kthread/uthread trampolines) right after their first switch */
void sched_tail(void) {
    switch_finish();
    int depth = current->bkl_depth;
    bkl_lock();
    reap();
    if (depth) this_cpu()->bkl_depth = depth;
    else bkl_unlock();                   /* user tasks go straight to user mode */
}

static inline void set_user_bases(task_t *prev, task_t *next) {
    if (prev->fs_base != next->fs_base) wrmsr(0xC0000100, next->fs_base);
    if (prev->gs_base != next->gs_base) wrmsr(0xC0000102, next->gs_base);   /* swapped in on return */
}

void schedule(void) {
    uint64_t f = irq_save();
    cpu_t *c = this_cpu();
    spin_lock(&sched_lock);
    task_t *prev = c->cur;
    if (prev->state == T_RUNNING) {
        prev->state = T_READY;
        if (prev != c->idle) rq_push(prev);
    }
    task_t *next = rq_pop();
    if (!next) next = c->idle;
    next->state = T_RUNNING;
    c->need_resched = false;
    c->slice = QUANTUM_MS;
    if (next == prev) {
        spin_unlock(&sched_lock);
        irq_restore(f);
        return;
    }
    prev->bkl_depth = bkl_release_all();
    c->cur = next;
    c->prev = prev;
    next->cpu = c->id;
    fxsave(prev->fpu);
    fxrstor(next->fpu);
    tss_set_rsp0(next->kstack_top);
    if (next->cr3 != read_cr3()) write_cr3(next->cr3);
    set_user_bases(prev, next);
    swtch(&prev->rsp, next->rsp);
    /* back again, possibly on another CPU */
    switch_finish();
    int depth = current->bkl_depth;
    if (depth) {
        bkl_reacquire(depth);
        reap();
    }
    irq_restore(f);
}

void yield(void) { schedule(); }

/* per-CPU timer tick: accounting and time slice */
void sched_tick_local(void) {
    cpu_t *c = this_cpu();
    task_t *cur = c->cur;
    if (!cur) return;
    cur->cpu_ms++;
    if (cur == c->idle) {
        c->idle_ms++;
        if (rq_head || rq_hi_head) c->need_resched = true;
    } else {
        c->busy_ms++;
        if (--c->slice <= 0) c->need_resched = true;
    }
}

/* global tick (boot CPU, BKL held): wake sleepers, per-second statistics */
void sched_tick_global(void) {
    for (task_t *t = sleepers, *n; t; t = n) {
        n = t->sleep_next;
        if (t->wake_at <= ticks) {
            t->timed_out = true;
            make_ready(t);
        }
    }
    static uint64_t last_snap;
    if (ticks - last_snap >= 1000) {
        uint64_t span = ticks - last_snap;
        last_snap = ticks;
        for (task_t *t = all_tasks; t; t = t->all_next) {
            /* percent of the whole machine, like most task managers */
            t->cpu_percent = (int)((t->cpu_ms - t->cpu_ms_snapshot) * 100 / (span * cpu_count));
            t->cpu_ms_snapshot = t->cpu_ms;
        }
        for (int i = 0; i < cpu_count; i++) {
            cpu_t *c = &cpus[i];
            uint64_t b = c->busy_ms - c->busy_snap, id = c->idle_ms - c->idle_snap;
            c->busy_snap = c->busy_ms;
            c->idle_snap = c->idle_ms;
            c->load = b + id ? (int)(b * 100 / (b + id)) : 0;
        }
    }
}

void proc_check_killed(regs_t *r);

void sched_trap_exit(regs_t *r) {
    cpu_t *c = this_cpu();
    if (c->need_resched && c->cur) schedule();
    if (regs_from_user(r) && current->killed) proc_check_killed(r);
}

static void add_sleeper(task_t *t, uint64_t ms) {
    t->wake_at = ticks + ms;
    t->timed_out = false;
    t->sleep_next = sleepers;
    sleepers = t;
    t->on_sleep_list = true;
}

void sleep_ms(uint64_t ms) {
    uint64_t f = irq_save();
    current->state = T_SLEEPING;
    add_sleeper(current, ms ? ms : 1);
    schedule();
    irq_restore(f);
}

void wq_wait(waitq_t *wq) {
    uint64_t f = irq_save();
    task_t *cur = current;
    cur->state = T_BLOCKED;
    cur->wq = wq;
    cur->wq_next = wq->head;
    wq->head = cur;
    schedule();
    irq_restore(f);
}

bool wq_wait_timeout(waitq_t *wq, uint64_t ms) {
    uint64_t f = irq_save();
    task_t *cur = current;
    cur->state = T_BLOCKED;
    cur->wq = wq;
    cur->wq_next = wq->head;
    wq->head = cur;
    add_sleeper(cur, ms);
    schedule();
    bool ok = !cur->timed_out;
    irq_restore(f);
    return ok;
}

void wq_wake_all(waitq_t *wq) {
    uint64_t f = irq_save();
    while (wq->head) {
        task_t *t = wq->head;
        make_ready(t);   /* removes from wq */
        if (wq->head == t) wq->head = t->wq_next;   /* safety */
    }
    irq_restore(f);
}

void wq_wake_one(waitq_t *wq) {
    uint64_t f = irq_save();
    /* wake the task that waited longest (tail of the list) */
    task_t *t = wq->head;
    while (t && t->wq_next) t = t->wq_next;
    if (t) make_ready(t);
    irq_restore(f);
}

void poll_notify(void) { wq_wake_all(&poll_wq); }

void mutex_lock(mutex_t *m) {
    uint64_t f = irq_save();
    while (m->owner && m->owner != current) wq_wait(&m->wq);
    m->owner = current;
    m->depth++;
    irq_restore(f);
}

void mutex_unlock(mutex_t *m) {
    uint64_t f = irq_save();
    if (m->owner == current && --m->depth == 0) {
        m->owner = 0;
        wq_wake_one(&m->wq);
    }
    irq_restore(f);
}

NORETURN void sched_exit(int state) {
    cli();
    current->state = state;
    schedule();
    panic("sched_exit: returned from schedule");
}

NORETURN void kthread_exit(int code) {
    current->exit_code = code;
    sched_exit(T_DEAD);
}

/* an idle task for an application processor (runs on the given stack-less task) */
task_t *sched_create_idle(cpu_t *c) {
    char name[16];
    snprintf(name, sizeof(name), "idle%d", c->id);
    task_t *t = task_alloc(name);
    t->state = T_RUNNING;
    t->cpu = c->id;
    c->idle = t;
    return t;
}

void sched_init(void) {
    /* the boot context becomes the idle task of CPU 0 */
    cpu_t *c = this_cpu();
    task_t *idle = task_alloc("idle0");
    vfree(idle->kstack);                 /* idle runs on the boot stack */
    extern char kernel_boot_stack_top[];
    idle->kstack = 0;
    idle->kstack_top = (uint64_t)kernel_boot_stack_top;
    idle->state = T_RUNNING;
    c->idle = idle;
    c->cur = idle;
    c->online = true;
    tss_set_rsp0(idle->kstack_top);
}

/* every CPU's idle task loops here */
NORETURN void sched_idle_loop(void) {
    for (;;) {
        cli();
        cpu_t *c = this_cpu();
        if (c->need_resched || rq_head || rq_hi_head) schedule();
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

__attribute__((weak)) void proc_release_resources(task_t *t) { UNUSED(t); }
__attribute__((weak)) void proc_check_killed(regs_t *r) { UNUSED(r); }
