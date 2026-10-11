#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"

#define WORKERS 8
#define ITERATIONS 20000

struct stats_ lwip_stats;

/* Production lwip_compat.c supplies heap/protect algorithms. Host seams only
 * replace spinlocks, IRQ state and CPU identity (one thread per CPU). */
__thread unsigned a20_host_cpu;
__thread int a20_host_irq_enabled = 1;

static struct pbuf *shared_pbuf;
static volatile int failed;

static void *worker(void *arg) {
    uintptr_t id = (uintptr_t)arg;
    a20_host_cpu = (unsigned)id + 1;
    uint32_t state = (uint32_t)(0x9e3779b9u ^ (uint32_t)id);
    unsigned i;

    for (i = 0; i < ITERATIONS; ++i) {
        int irq_before = (i & 7u) != 0;
        a20_host_irq_enabled = irq_before;
        size_t len;
        unsigned char value;
        unsigned char *p;
        size_t j;
        void *pool_element;

        state = state * 1664525u + 1013904223u;
        len = 1 + ((state >> 8) % 1536);
        value = (unsigned char)(id * 19u + i);
        p = (unsigned char *)mem_malloc((mem_size_t)len);
        if (p == NULL) {
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            return NULL;
        }
        memset(p, value, len);

        /* Exercise real heap split/shrink/coalesce paths, not a mock allocator. */
        if (len > 64 && (i & 3u) == 0) {
            size_t shrunk = len / 2;
            if (mem_trim(p, (mem_size_t)shrunk) != p) {
                __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
                mem_free(p);
                return NULL;
            }
            len = shrunk;
        }
        for (j = 0; j < len; ++j) {
            if (p[j] != value) {
                __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
                break;
            }
        }
        mem_free(p);

        struct pbuf *private = pbuf_alloc(PBUF_TRANSPORT, 128, PBUF_RAM);
        if (!private || private->ref != 1) {
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            return NULL;
        }
        memset(private->payload, value, private->len);
        pbuf_free(private);

        pool_element = memp_malloc(MEMP_TCP_SEG);
        if (pool_element == NULL) {
            __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            return NULL;
        }
        memp_free(MEMP_TCP_SEG, pool_element);

        /* Keep one owning reference in main so concurrent ref/free pairs
         * exercise the actual pbuf refcount without allowing final release. */
        pbuf_ref(shared_pbuf);
        pbuf_free(shared_pbuf);
        if (a20_host_irq_enabled != irq_before)
            abort();

        sys_prot_t outer = sys_arch_protect(), inner = sys_arch_protect();
        if (a20_host_irq_enabled)
            abort();
        sys_arch_unprotect(inner);
        if (a20_host_irq_enabled)
            abort();
        sys_arch_unprotect(outer);
        if (a20_host_irq_enabled != irq_before)
            abort();
        a20_host_irq_enabled = 1;
    }
    return NULL;
}

int main(void) {
    pthread_t threads[WORKERS];
    unsigned i;
    unsigned final_ref;

    mem_init();
    memp_init();
    shared_pbuf = pbuf_alloc(PBUF_RAW, 128, PBUF_RAM);
    if (shared_pbuf == NULL) {
        fprintf(stderr, "pbuf_alloc failed before test\n");
        return 1;
    }

    for (i = 0; i < WORKERS; ++i) {
        if (pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i) !=
            0) {
            perror("pthread_create");
            return 1;
        }
    }
    for (i = 0; i < WORKERS; ++i)
        pthread_join(threads[i], NULL);

    final_ref = shared_pbuf->ref;
    pbuf_free(shared_pbuf);
    if (__atomic_load_n(&failed, __ATOMIC_RELAXED) || final_ref != 1 ||
        lwip_stats.memp[MEMP_TCP_SEG]->used != 0 || lwip_stats.mem.used != 0 ||
        lwip_stats.mem.err != 0) {
        fprintf(
            stderr,
            "allocator concurrency failure: failed=%d ref=%u tcp_seg_used=%u "
            "mem_used=%lu mem_err=%u\n",
            failed, final_ref, (unsigned)lwip_stats.memp[MEMP_TCP_SEG]->used,
            (unsigned long)lwip_stats.mem.used, (unsigned)lwip_stats.mem.err);
        return 1;
    }
    printf("lwIP allocator concurrency: PASS (%u lanes, %u workers x %u "
           "iterations)\n",
           CONFIG_NET_LANES, WORKERS, ITERATIONS);
    return 0;
}
