#pragma once
/* POSIX threads (subset) on top of ClaudeOS kernel threads and futexes */
#include <stddef.h>
#include <stdint.h>

typedef struct __pthread *pthread_t;

typedef struct {
    size_t stack_size;
    int detached;
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_DEFAULT 0

typedef struct { int type; } pthread_mutexattr_t;
typedef struct {
    volatile int lock;          /* 0 free, 1 locked, 2 locked with waiters */
    int type;
    volatile int owner;
    int count;
} pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER { 0, 0, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER { 0, 1, 0, 0 }

typedef struct { int unused; } pthread_condattr_t;
typedef struct { volatile int seq; } pthread_cond_t;
#define PTHREAD_COND_INITIALIZER { 0 }

typedef struct { volatile int state; } pthread_once_t;
#define PTHREAD_ONCE_INIT { 0 }

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    unsigned count, needed, phase;
} pthread_barrier_t;
typedef int pthread_barrierattr_t;
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_detach(pthread_t t);
void pthread_exit(void *ret) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);

int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, long ms);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_once(pthread_once_t *o, void (*fn)(void));

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_barrier_destroy(pthread_barrier_t *b);

int sched_yield(void);
int get_nprocs(void);           /* number of online CPUs */
int gettid(void);

/* low-level futex-based lock used inside libc */
void __lock(volatile int *l);
void __unlock(volatile int *l);
