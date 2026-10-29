/* Production allocator adapter; host hooks inject failure and track backing.
 * No BSD function is stubbed to return success. */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "kernel/net/backends/bsd/runtime/memory.h"
#include "ff_host_interface.h"

#include "bsd_runtime_host/hooks.h"
static void empty(void)
{
    struct bsd_memory_stats s;
    port_memory_snapshot(&s);
    assert(s.used == 0 && s.allocations == 0);
    assert(backing_bytes == 0 && backing_count == 0);
    assert(s.peak <= s.limit);
}
static void *worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    for (unsigned i = 0; i < 4000; ++i) {
        size_t n = 1 + (i * 31 + id * 17) % 2048;
        unsigned char *p = ff_malloc(n);
        if (!p) continue; /* Contention may legitimately reach the hard cap. */
        assert((uintptr_t)p % 16 == 0);
        memset(p, (int)id, n);
        for (size_t j = 0; j < n; ++j) assert(p[j] == id);
        ff_free(p);
    }
    return NULL;
}
int main(void)
{
    empty();
    void *zero = ff_malloc(0);
    assert(zero && (uintptr_t)zero % 16 == 0);
    ff_free(zero); ff_free(NULL);
    unsigned char *p = ff_calloc(17, 31);
    assert(p);
    for (size_t i = 0; i < 527; ++i) assert(p[i] == 0);
    memset(p, 0x63, 527);
    unsigned char *q = ff_realloc(p, 1024);
    assert(q);
    for (size_t i = 0; i < 527; ++i) assert(q[i] == 0x63);
    p = ff_realloc(q, 31);
    assert(p);
    for (unsigned i = 0; i < 31; ++i) assert(p[i] == 0x63);
    atomic_store(&fail_next, 1);
    assert(ff_realloc(p, 300) == NULL);
    for (unsigned i = 0; i < 31; ++i) assert(p[i] == 0x63);
    assert(ff_realloc(p, 65536) == NULL);
    for (unsigned i = 0; i < 31; ++i) assert(p[i] == 0x63);
    assert(ff_realloc(p, 0) == NULL);
    assert(ff_calloc(UINT64_MAX, 2) == NULL);
    assert(ff_malloc(UINT64_MAX) == NULL);
    empty();
    p = ff_mmap(NULL, 8192, 3, ff_MAP_ANON | ff_MAP_PRIVATE, -1, 0);
    assert(p && (uintptr_t)p % 4096 == 0);
    for (unsigned i = 0; i < 8192; ++i) assert(p[i] == 0);
    memset(p, 0x71, 8192);
    struct bsd_memory_stats before, after;
    port_memory_snapshot(&before);
    assert(before.allocations == 1 && before.used == backing_bytes);
    assert(ff_munmap(p, 4096) == -1);
    port_memory_snapshot(&after);
    assert(before.used == after.used && after.allocations == 1);
    for (unsigned i = 0; i < 8192; ++i) assert(p[i] == 0x71);
    assert(ff_munmap(p, 8192) == 0);
    assert(ff_mmap(NULL, 4096, 3, ff_MAP_ANON | ff_MAP_SHARED, -1, 0) == NULL);
    assert(ff_mmap(NULL, 4096, ff_PROT_READ, ff_MAP_ANON | ff_MAP_PRIVATE, -1, 0) == NULL);
    assert(ff_mmap(NULL, 4095, 3, ff_MAP_ANON | ff_MAP_PRIVATE, -1, 0) == NULL);
    assert(ff_mmap(NULL, 4096, 3, ff_MAP_ANON | ff_MAP_PRIVATE, 1, 0) == NULL);
    atomic_store(&fail_next, 1);
    assert(ff_mmap(NULL, 4096, 3, ff_MAP_ANON | ff_MAP_PRIVATE, -1, 0) == NULL);
    empty();
    p = ff_malloc(50000);
    assert(p && ff_malloc(20000) == NULL);
    ff_free(p);
    empty();
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, worker, (void *)(i + 1)));
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    empty();
    puts("BSD_MEMORY_HOST_PASS: rollback, zeroing, alignment, cap, concurrent reclamation");
    return 0;
}
