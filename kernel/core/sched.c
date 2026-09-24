/* Preemptive round-robin scheduler, kernel threads, wait queues, mutexes */
#include <kernel.h>
#include <sched.h>
#include <cpu.h>
#include <mm.h>

task_t *current;
volatile bool need_resched;
waitq_t poll_wq;

static task_t *rq_head, *rq_tail;
static task_t *rq_hi_head, *rq_hi_tail;
static task_t *sleepers;
static task_t *all_tasks;
static task_t *idle_task;
static task_t *reap_list;
static int next_pid = 1;
static int slice;
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

/* make a blocked/sleeping task runnable (interrupts must be disabled) */
static void make_ready(task_t *t) {
    if (t->state != T_BLOCKED && t->state != T_SLEEPING) return;
    sleep_list_remove(t);
    wq_remove(t);
    t->state = T_READY;
    rq_push(t);
    if (t->prio > current->prio || current == idle_task) need_resched = true;
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
    uint64_t f = irq_save();
    t->state = T_READY;
    rq_push(t);
    irq_restore(f);
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
    sched_add(t);
    return t;
}

/* free resources of tasks that died (never the current one) */
void proc_release_resources(task_t *t);
static void reap(void) {
    while (reap_list) {
        task_t *t = reap_list;
        reap_list = t->rq_next;
        if (t->is_user) {
            proc_release_resources(t);
            /* the task struct stays as zombie until waited for */
            vfree(t->kstack);
            t->kstack = 0;
        } else {
            task_free(t);
        }
    }
}

void sched_tail(void) {
    reap();
}

void schedule(void) {
    uint64_t f = irq_save();
    task_t *prev = current;
    if (prev->state == T_RUNNING) {
        prev->state = T_READY;
        if (prev != idle_task) rq_push(prev);
    }
    task_t *next = rq_pop();
    if (!next) next = idle_task;
    next->state = T_RUNNING;
    need_resched = false;
    slice = QUANTUM_MS;
    if (next != prev) {
        current = next;
        fxsave(prev->fpu);
        fxrstor(next->fpu);
        tss_set_rsp0(next->kstack_top);
        if (next->cr3 != read_cr3()) write_cr3(next->cr3);
        swtch(&prev->rsp, next->rsp);
        reap();
    }
    irq_restore(f);
}

void yield(void) { schedule(); }

void sched_tick(void) {
    if (!current) return;
    current->cpu_ms++;
    for (task_t *t = sleepers, *n; t; t = n) {
        n = t->sleep_next;
        if (t->wake_at <= ticks) {
            t->timed_out = true;
            make_ready(t);
        }
    }
    if (current == idle_task) {
        if (rq_head || rq_hi_head) need_resched = true;
    } else if (--slice <= 0) {
        need_resched = true;
    }
    /* per-second cpu usage snapshot */
    static uint64_t last_snap;
    if (ticks - last_snap >= 1000) {
        uint64_t span = ticks - last_snap;
        last_snap = ticks;
        for (task_t *t = all_tasks; t; t = t->all_next) {
            t->cpu_percent = (int)((t->cpu_ms - t->cpu_ms_snapshot) * 100 / span);
            t->cpu_ms_snapshot = t->cpu_ms;
        }
    }
}

void proc_check_killed(regs_t *r);

void sched_trap_exit(regs_t *r) {
    if (need_resched && current) schedule();
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
    current->state = T_BLOCKED;
    current->wq = wq;
    current->wq_next = wq->head;
    wq->head = current;
    schedule();
    irq_restore(f);
}

bool wq_wait_timeout(waitq_t *wq, uint64_t ms) {
    uint64_t f = irq_save();
    current->state = T_BLOCKED;
    current->wq = wq;
    current->wq_next = wq->head;
    wq->head = current;
    add_sleeper(current, ms);
    schedule();
    bool ok = !current->timed_out;
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
    current->rq_next = reap_list;
    reap_list = current;
    schedule();
    panic("sched_exit: returned from schedule");
}

NORETURN void kthread_exit(int code) {
    current->exit_code = code;
    sched_exit(T_DEAD);
}

void sched_init(void) {
    /* the boot context becomes the idle task */
    idle_task = task_alloc("idle");
    vfree(idle_task->kstack);            /* idle runs on the boot stack */
    extern char kernel_boot_stack[], kernel_boot_stack_top[];
    idle_task->kstack = 0;
    idle_task->kstack_top = (uint64_t)kernel_boot_stack_top;
    idle_task->state = T_RUNNING;
    current = idle_task;
    tss_set_rsp0(idle_task->kstack_top);
}

/* the boot/idle task loops here once init is complete */
NORETURN void sched_idle_loop(void) {
    for (;;) {
        sti();
        hlt();
        if (need_resched) schedule();
    }
}

__attribute__((weak)) void proc_release_resources(task_t *t) { UNUSED(t); }
__attribute__((weak)) void proc_check_killed(regs_t *r) { UNUSED(r); }
