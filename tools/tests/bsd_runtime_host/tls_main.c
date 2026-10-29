#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "hooks.h"
#include "kernel/net/backends/bsd/runtime/tls.h"
#include "kernel/net/backends/bsd/runtime/memory.h"
unsigned *probe_counter(void);
unsigned char *probe_data(void);
static void *worker(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    struct port_tls_context *context = port_tls_create();
    assert(context && port_tls_bind(context) == 0);
    assert(port_tls_bind(context) == -16);
    assert(port_tls_destroy(context) == -16);
    assert(*probe_counter() == 17);
    unsigned char *bytes = probe_data();
    assert((uintptr_t)bytes % 64 == 0);
    for (unsigned j = 0; j < 257; ++j) assert(bytes[j] == 0);
    memset(bytes, (int)id, 257);
    host_canary_set(0xf0123000 + id);
    unsigned *counter = probe_counter();
    for (unsigned i = 0; i < 4096; ++i) {
        *counter = id * 100000 + i;
        assert(probe_counter() == counter && *probe_counter() == id * 100000 + i);
        for (unsigned j = 0; j < 257; ++j) assert(probe_data()[j] == id);
        assert(host_canary_get() == 0xf0123000 + id);
    }
    assert(port_tls_unbind(context) == 0);
    assert(port_tls_unbind(context) == -22);
    assert(port_tls_destroy(context) == 0);
    return NULL;
}
static pthread_barrier_t capacity_barrier;
static void *capacity_worker(void *unused)
{
    (void)unused;
    struct port_tls_context *context = port_tls_create();
    assert(context && port_tls_bind(context) == 0);
    pthread_barrier_wait(&capacity_barrier);
    pthread_barrier_wait(&capacity_barrier);
    assert(port_tls_unbind(context) == 0 && port_tls_destroy(context) == 0);
    return NULL;
}
static void *wrong_owner(void *argument)
{
    struct port_tls_context *context = argument;
    assert(port_tls_bind(context) == -16);
    assert(port_tls_unbind(context) == -22);
    assert(port_tls_destroy(context) == -16);
    return NULL;
}
int main(void)
{
    /* Access without a bound owner must fail, rather than invent thread0. */
    pid_t child = fork();
    assert(child >= 0);
    if (!child) { (void)probe_counter(); _exit(1); }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    pthread_t threads[4];
    for (uintptr_t i = 1; i <= 4; ++i) assert(!pthread_create(&threads[i-1], NULL, worker, (void *)i));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    struct port_tls_context *owned = port_tls_create();
    assert(owned && port_tls_bind(owned) == 0);
    pthread_t wrong;
    assert(!pthread_create(&wrong, NULL, wrong_owner, owned));
    assert(!pthread_join(wrong, NULL));
    assert(port_tls_unbind(owned) == 0 && port_tls_destroy(owned) == 0);
    pthread_t full[8];
    assert(!pthread_barrier_init(&capacity_barrier, NULL, 9));
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&full[i], NULL, capacity_worker, NULL));
    pthread_barrier_wait(&capacity_barrier);
    struct port_tls_context *extra = port_tls_create();
    assert(extra && port_tls_bind(extra) == -28 && port_tls_destroy(extra) == 0);
    pthread_barrier_wait(&capacity_barrier);
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(full[i], NULL));
    assert(!pthread_barrier_destroy(&capacity_barrier));
    struct bsd_memory_stats s;
    port_memory_snapshot(&s);
    assert(s.used == 0 && s.allocations == 0 && backing_bytes == 0 && backing_count == 0);
    puts("BSD_TLS_HOST_PASS: compiler TLS, four owners, initialized/aligned data, user TLS intact, reclamation");
    return 0;
}
