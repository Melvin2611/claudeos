/* POSIX threads on ClaudeOS: kernel threads share the address space, the FS base
 * points at the thread control block (per-thread errno), futexes do the waiting. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>

struct __pthread {
    struct __pthread *self;     /* %fs:0 */
    int err;                    /* per-thread errno */
    volatile int tid;           /* cleared by the kernel when the thread exits */
    void *(*fn)(void *);
    void *arg;
    void *ret;
    void *stack;
    int detached;
};

static struct __pthread main_thread;

static long futex(volatile int *addr, int op, int val, long timeout_ms) {
    return syscall4(SYS_FUTEX, addr, op, val, timeout_ms);
}

void __libc_init_threads(void) {
    main_thread.self = &main_thread;
    main_thread.tid = (int)syscall0(SYS_GETTID);
    syscall1(SYS_SET_FS, &main_thread);
}

pthread_t pthread_self(void) {
    struct __pthread *t;
    __asm__("mov %%fs:0, %0" : "=r"(t));
    return t;
}

int *__errno_location(void) {
    return &pthread_self()->err;
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
int gettid(void) { return (int)syscall0(SYS_GETTID); }

int get_nprocs(void) {
    long n = syscall2(SYS_CPUINFO, 0, 0);
    return n > 0 ? (int)n : 1;
}

/* ------------------------------------------------------------------ internal lock (Drepper's mutex) */

void __lock(volatile int *l) {
    int c = __sync_val_compare_and_swap(l, 0, 1);
    if (c == 0) return;
    do {
        if (c == 2 || __sync_val_compare_and_swap(l, 1, 2) != 0) futex(l, FUTEX_WAIT, 2, -1);
    } while ((c = __sync_val_compare_and_swap(l, 0, 2)) != 0);
}

void __unlock(volatile int *l) {
    if (__sync_fetch_and_sub(l, 1) != 1) {
        *l = 0;
        futex(l, FUTEX_WAKE, 1, 0);
    }
}

/* ------------------------------------------------------------------ threads */

#define DEFAULT_STACK (256 * 1024)

static void thread_start(struct __pthread *t) {
    t->ret = t->fn(t->arg);
    pthread_exit(t->ret);
}

int pthread_attr_init(pthread_attr_t *a) { a->stack_size = DEFAULT_STACK; a->detached = 0; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) {
    if (size < 16384) return EINVAL;
    a->stack_size = size;
    return 0;
}
int pthread_attr_setdetachstate(pthread_attr_t *a, int state) { a->detached = state; return 0; }

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    size_t ss = attr ? attr->stack_size : DEFAULT_STACK;
    struct __pthread *t = calloc(1, sizeof(*t));
    if (!t) return EAGAIN;
    t->stack = malloc(ss);
    if (!t->stack) { free(t); return EAGAIN; }
    t->self = t;
    t->fn = fn;
    t->arg = arg;
    t->detached = attr ? attr->detached : 0;
    t->tid = -1;
    long r = syscall5(SYS_THREAD_CREATE, thread_start, t, (char *)t->stack + ss, &t->tid, t);
    if (r < 0) {
        free(t->stack);
        free(t);
        return (int)-r;
    }
    *out = t;
    return 0;
}

void pthread_exit(void *ret) {
    struct __pthread *t = pthread_self();
    t->ret = ret;
    if (t == &main_thread) exit(0);
    for (;;) syscall1(SYS_THREAD_EXIT, 0);
}

int pthread_join(pthread_t t, void **ret) {
    if (t == pthread_self()) return EDEADLK;
    int tid;
    while ((tid = t->tid) != 0) futex(&t->tid, FUTEX_WAIT, tid, -1);
    if (ret) *ret = t->ret;
    free(t->stack);
    free(t);
    return 0;
}

int pthread_detach(pthread_t t) {
    /* the stack cannot be freed by the exiting thread itself; it is leaked */
    t->detached = 1;
    return 0;
}

/* ------------------------------------------------------------------ mutexes */

int pthread_mutexattr_init(pthread_mutexattr_t *a) { a->type = 0; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type) { a->type = type; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *a) { (void)a; return 0; }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    memset(m, 0, sizeof(*m));
    m->type = a ? a->type : 0;
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_mutex_lock(pthread_mutex_t *m) {
    int me = pthread_self()->tid;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->owner == me) { m->count++; return 0; }
    __lock(&m->lock);
    m->owner = me;
    m->count = 1;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    int me = pthread_self()->tid;
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->owner == me) { m->count++; return 0; }
    if (__sync_val_compare_and_swap(&m->lock, 0, 1) != 0) return EBUSY;
    m->owner = me;
    m->count = 1;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count > 0) return 0;
    m->owner = 0;
    __unlock(&m->lock);
    return 0;
}

/* ------------------------------------------------------------------ condition variables */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)a; c->seq = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

int pthread_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, long ms) {
    int seq = c->seq;
    int count = m->count;
    m->count = 1;
    pthread_mutex_unlock(m);
    long r = futex(&c->seq, FUTEX_WAIT, seq, ms);
    pthread_mutex_lock(m);
    m->count = count;
    return r == -ETIMEDOUT ? ETIMEDOUT : 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return pthread_cond_timedwait_ms(c, m, -1); }

int pthread_cond_signal(pthread_cond_t *c) {
    __sync_fetch_and_add(&c->seq, 1);
    futex(&c->seq, FUTEX_WAKE, 1, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    __sync_fetch_and_add(&c->seq, 1);
    futex(&c->seq, FUTEX_WAKE, 0x7FFFFFFF, 0);
    return 0;
}

/* ------------------------------------------------------------------ once / barrier */

int pthread_once(pthread_once_t *o, void (*fn)(void)) {
    if (o->state == 2) return 0;
    if (__sync_val_compare_and_swap(&o->state, 0, 1) == 0) {
        fn();
        o->state = 2;
        futex(&o->state, FUTEX_WAKE, 0x7FFFFFFF, 0);
        return 0;
    }
    while (o->state != 2) futex(&o->state, FUTEX_WAIT, 1, -1);
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count) {
    (void)a;
    if (!count) return EINVAL;
    memset(b, 0, sizeof(*b));
    b->needed = count;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b) {
    pthread_mutex_lock(&b->m);
    unsigned phase = b->phase;
    if (++b->count == b->needed) {
        b->count = 0;
        b->phase++;
        pthread_cond_broadcast(&b->c);
        pthread_mutex_unlock(&b->m);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (phase == b->phase) pthread_cond_wait(&b->c, &b->m);
    pthread_mutex_unlock(&b->m);
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b) { (void)b; return 0; }
