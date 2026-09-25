/* mtbench - multi-core benchmark and thread library self-test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <claudeos.h>

#define WORK 200000000UL

typedef struct { unsigned long from, to; double result; } job_t;

static void *work(void *arg) {
    job_t *j = arg;
    double s = 0;
    for (unsigned long i = j->from; i < j->to; i++) s += 1.0 / ((double)i * i + 1.0);
    j->result = s;
    return 0;
}

static double run(int nthreads, uint64_t *ms) {
    pthread_t th[64];
    job_t jobs[64];
    uint64_t t0 = uptime_ms();
    for (int i = 0; i < nthreads; i++) {
        jobs[i].from = WORK / nthreads * i;
        jobs[i].to = i == nthreads - 1 ? WORK : WORK / nthreads * (i + 1);
        pthread_create(&th[i], 0, work, &jobs[i]);
    }
    double s = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(th[i], 0);
        s += jobs[i].result;
    }
    *ms = uptime_ms() - t0 + 1;
    return s;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static long counter;
static int ready;

static void *adder(void *arg) {
    (void)arg;
    for (int i = 0; i < 100000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return 0;
}

static void *waiter(void *arg) {
    pthread_mutex_lock(&lock);
    while (!ready) pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    return arg;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : get_nprocs();
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    printf("CPUs online: %d\n", get_nprocs());

    /* correctness: mutex-protected counter */
    pthread_t th[8];
    for (int i = 0; i < 8; i++) pthread_create(&th[i], 0, adder, 0);
    for (int i = 0; i < 8; i++) pthread_join(th[i], 0);
    printf("mutex test:   counter = %ld (expected 800000) %s\n", counter, counter == 800000 ? "OK" : "FAILED");

    /* condition variable + return values */
    for (int i = 0; i < 4; i++) pthread_create(&th[i], 0, waiter, (void *)(long)(i + 1));
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    long sum = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); sum += (long)r; }
    printf("condvar test: sum = %ld (expected 10) %s\n", sum, sum == 10 ? "OK" : "FAILED");

    uint64_t t1, tn;
    double r1 = run(1, &t1);
    double rn = run(n, &tn);
    printf("1 thread:   %6lu ms  (result %.9f)\n", (unsigned long)t1, r1);
    printf("%d threads: %6lu ms  (result %.9f)\n", n, (unsigned long)tn, rn);
    printf("speed-up:   %.2fx\n", (double)t1 / tn);
    return 0;
}
