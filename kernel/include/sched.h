#pragma once
#include <kernel.h>
#include <smp.h>

#define MAX_FDS 64
#define KSTACK_SIZE (32 * 1024)

enum { T_READY, T_RUNNING, T_BLOCKED, T_SLEEPING, T_ZOMBIE, T_DEAD };

struct task;
struct file;

typedef struct waitq {
    struct task *head;
} waitq_t;

typedef struct task {
    int pid, ppid;
    char name[32];
    volatile int state;
    uint64_t rsp;                 /* saved kernel stack pointer */
    uint8_t *kstack;
    uint64_t kstack_top;
    uint8_t *fpu;                 /* 512 bytes, 16-byte aligned */
    void *fpu_alloc;
    uint64_t cr3;
    bool is_user;
    int prio;                     /* 0 normal, 1 high (window server) */
    int cpu;                      /* CPU the task last ran on */
    int bkl_depth;                /* big kernel lock depth saved across switches */
    uint64_t fs_base, gs_base;    /* user FS/GS base (TLS) */

    /* threads: process-wide state lives in the thread group leader */
    struct task *leader;          /* self for single-threaded processes and kernel threads */
    int nthreads;                 /* leader: live (not yet reaped) threads including itself */
    waitq_t thread_wq;            /* leader: woken whenever a thread is reaped */
    bool group_exit;              /* leader: the process is exiting */
    uint64_t clear_tid;           /* user address zeroed + futex-woken when the thread exits */
    uint64_t futex_key, futex_cr3;

    /* scheduling bookkeeping */
    uint64_t wake_at;
    bool timed_out;
    bool on_sleep_list;
    struct task *rq_next, *sleep_next, *wq_next, *all_next;
    waitq_t *wq;
    uint64_t cpu_ms, start_ms, cpu_ms_snapshot;
    int cpu_percent;

    /* process state */
    int exit_code;
    volatile bool killed;
    bool waited;
    waitq_t child_wq;
    uint64_t brk_start, brk;
    uint64_t mmap_next;
    uint64_t stack_low;
    uint64_t user_pages;
    struct file *fds[MAX_FDS];
    char cwd[256];
    void *gui;                    /* window-server client data */
    char **env;                   /* unused in kernel, kept by spawn */
} task_t;

#define current ((task_t *)get_current())
#define PROC(t) ((t)->leader)

void sched_init(void);
task_t *kthread_create(const char *name, int (*fn)(void *), void *arg);
void sched_add(task_t *t);
void schedule(void);
void yield(void);
void sleep_ms(uint64_t ms);
NORETURN void kthread_exit(int code);
NORETURN void sched_exit(int state);   /* T_DEAD (free all) or T_ZOMBIE (keep struct) */
NORETURN void sched_idle_loop(void);
task_t *sched_create_idle(cpu_t *c);
void sched_tick_local(void);
void sched_tick_global(void);
task_t *task_alloc(const char *name);
void task_free(task_t *t);
task_t *task_find(int pid);
task_t *task_list(void);              /* head of all-task list */

/* wait queues: call with interrupts disabled (irq_save) around the condition check */
void wq_wait(waitq_t *wq);                            /* sleep until woken */
bool wq_wait_timeout(waitq_t *wq, uint64_t ms);       /* false on timeout */
void wq_wake_all(waitq_t *wq);
void wq_wake_one(waitq_t *wq);
void task_wake(task_t *t);

/* sleeping mutex (recursive) */
typedef struct {
    task_t *owner;
    int depth;
    waitq_t wq;
} mutex_t;
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* global poll wakeup (any pollable object changed state) */
extern waitq_t poll_wq;
void poll_notify(void);
