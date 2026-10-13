/* Full libc exercise on the Native ABI. No thread placement or affinity set. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "liba20rt/a20_task.h"
#include "liba20rt/a20_execution.h"

extern a20_start_info_t *__a20_start_info;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned ready, go, next, jobs[2], sums[2], masks[2], completed;
static unsigned results[32];
static _Thread_local unsigned tls_word;
static const unsigned char input[4096] = {[0] = 0xa2, [4095] = 0x20};

static void checked(int error) {
    if (error) {
        fprintf(stderr, "MLIBC_THREADS: FAIL pthread error=%d\n", error);
        exit(1);
    }
}

static void *worker(void *argument) {
    unsigned part = (uintptr_t)argument;
    tls_word = 0xa200 + part;
    checked(pthread_mutex_lock(&lock));
    ready++;
    checked(pthread_cond_broadcast(&changed));
    while (!go)
        checked(pthread_cond_wait(&changed, &lock));
    checked(pthread_mutex_unlock(&lock));

    for (;;) {
        checked(pthread_mutex_lock(&lock));
        unsigned job = next++;
        checked(pthread_mutex_unlock(&lock));
        if (job >= 32)
            break;
        /* Enough real computation for idle CPUs to observe runnable work.
         * This is a correctness workload, with no duration/throughput target. */
        unsigned hash = 2166136261U;
        for (unsigned pass = 0; pass < 128; pass++)
            for (unsigned i = 0; i < sizeof(input); i++)
                hash = (hash ^ input[i]) * 16777619U;
        unsigned sum = 0;
        for (unsigned i = 1; i <= 128; i++)
            sum += i * (job + 1);
        uint32_t cpu;
        if (hash != 1339862469U || sum != 8256U * (job + 1) ||
            tls_word != 0xa200 + part || a20_thread_get_cpu(&cpu) || cpu >= 32)
            return NULL;
        results[job] = sum;
        jobs[part]++;
        sums[part] += sum;
        masks[part] |= 1U << cpu;
        checked(pthread_mutex_lock(&lock));
        if (completed & (1U << job)) {
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        completed |= 1U << job;
        checked(pthread_mutex_unlock(&lock));
        if (a20_thread_yield())
            return NULL;
    }
    return (void *)(uintptr_t)(37 + part);
}

int main(void) {
    a20_execution_domain_info_t domain;
    a20_sched_args_t scheduling;
    if (a20_execution_domain_info(&domain) ||
        !a20_execution_domain_valid(&domain) ||
        a20_sched_get(__a20_start_info->self_task, &scheduling))
        return 1;
    unsigned eligible = domain.online_mask & (uint32_t)scheduling.affinity;
    printf("MLIBC_THREADS: start online=%08x allowed=%08x\n",
           domain.online_mask, (unsigned)scheduling.affinity);
    fflush(stdout);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++)
        checked(
            pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    checked(pthread_mutex_lock(&lock));
    while (ready != 2)
        checked(pthread_cond_wait(&changed, &lock));
    go = 1;
    /* Holding the mutex ensures workers have committed their condition waits. */
    checked(pthread_cond_broadcast(&changed));
    checked(pthread_mutex_unlock(&lock));
    for (unsigned i = 0; i < 2; i++) {
        void *result = NULL;
        checked(pthread_join(threads[i], &result));
        if (result != (void *)(uintptr_t)(37 + i))
            return 2;
    }
    unsigned mask = masks[0] | masks[1];
    printf("MLIBC_THREADS: work jobs=%u sum=%u mask=%08x workers=%u/%u\n",
           jobs[0] + jobs[1], sums[0] + sums[1], mask, jobs[0], jobs[1]);
    if (!eligible || !jobs[0] || !jobs[1] || jobs[0] + jobs[1] != 32 ||
        sums[0] + sums[1] != 4359168 || completed != UINT32_MAX || !mask ||
        (mask & ~eligible) ||
        ((eligible & (eligible - 1)) && !(mask & (mask - 1))))
        return 3;
    for (unsigned job = 0; job < 32; job++)
        if (results[job] != 8256U * (job + 1))
            return 4;
    puts("MLIBC_THREADS: PASS pthread mutex cond TLS unbound actual-work");
    return 0;
}
