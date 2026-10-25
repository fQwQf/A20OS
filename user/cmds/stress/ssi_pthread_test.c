/* This is an ordinary pthread program: no cluster headers, calls or remote
 * page APIs. The same ELF runs directly, or through an SSI-configured exec. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local uint64_t tls_value;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t barrier;
static unsigned counter;
static uint64_t shared[4];
static uint64_t *heap;
static double fp[4];
static void *work(void *arg) {
    uintptr_t id = (uintptr_t)arg;
    tls_value = 0x12340000 + id;
    pthread_barrier_wait(&barrier);
    double f = 1.0 + id;
    for (unsigned pass = 0; pass < 5; pass++) {
        uint64_t sum = 0;
        for (unsigned i = 0; i < 2048; i++) {
            heap[id * 2048 + i] = i * 7 + tls_value + pass;
            sum += heap[id * 2048 + i];
            f = f * 1.000001 + 0.000001;
        }
        for (unsigned i = 0; i < 2000000; i++)
            f = f * 1.000000001 + 0.000000001;
        pthread_mutex_lock(&mutex);
        shared[id] = sum;
        __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&mutex);
        /* Syscall transitions must preserve TLS, FPU and shared memory. */
        if (getpid() <= 0 || tls_value != 0x12340000 + id)
            return (void *)1;
    }
    fp[id] = f;
    return (void *)(uintptr_t)tls_value;
}
int main(void) {
    pthread_t threads[4];
    heap = calloc(4 * 2048, sizeof(*heap));
    if (!heap || pthread_barrier_init(&barrier, NULL, 4))
        return 1;
    for (uintptr_t i = 0; i < 4; i++)
        if (pthread_create(&threads[i], NULL, work, (void *)i))
            return 2;
    for (uintptr_t i = 0; i < 4; i++) {
        void *result;
        if (pthread_join(threads[i], &result) ||
            (uintptr_t)result != 0x12340000 + i)
            goto fail;
        uint64_t expect = 0;
        for (unsigned j = 0; j < 2048; j++) {
            uint64_t v = j * 7 + 0x12340000 + i + 4;
            if (heap[i * 2048 + j] != v)
                goto fail;
            expect += v;
        }
        if (shared[i] != expect || !(fp[i] > 1.0 + i && fp[i] < 5.0 + i))
            goto fail;
    }
    if (__atomic_load_n(&counter, __ATOMIC_SEQ_CST) != 20)
        goto fail;
    puts("SSI_PTHREAD: PASS heap stack TLS atomics mutex barrier FPU join");
    free(heap);
    return 0;
fail:
    puts("SSI_PTHREAD: FAIL");
    return 3;
}
