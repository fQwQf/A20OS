#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "mm/flat_user.h"
#include "core/errno.h"

static int outstanding, fail_after = -1;
void *kcalloc(size_t count, size_t size) {
    if (fail_after >= 0 && fail_after-- == 0)
        return NULL;
    void *p = calloc(count, size);
    if (p)
        __atomic_add_fetch(&outstanding, 1, __ATOMIC_RELAXED);
    return p;
}
void kfree(void *p) {
    if (p) {
        __atomic_sub_fetch(&outstanding, 1, __ATOMIC_RELAXED);
        free(p);
    }
}
static a20_flat_space_t space;
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 200; i++) {
        uintptr_t address;
        assert(a20_flat_alloc(&space, 1 + i, &address) == 0);
        uint32_t value = i + 37, read = 0;
        assert(a20_flat_copy(&space, &value, (void *)address, 4, 1) == 0);
        a20_flat_region_t *pin = a20_flat_pin(&space, address, 4);
        assert(pin);
        assert(a20_flat_unmap(&space, address, 1 + i) == -EBUSY);
        assert(a20_flat_copy(&space, &read, (void *)address, 4, 0) == 0 &&
               read == value);
        a20_flat_unpin(&space, pin);
        assert(a20_flat_unmap(&space, address, 1 + i) == 0);
    }
    return NULL;
}
int main(void) {
    a20_flat_init(&space);
    unsigned char fixed[256] = {0};
    a20_flat_region_t permanent, overlap;
    assert(a20_flat_add_fixed(&space, &permanent, (uintptr_t)fixed,
                              sizeof(fixed)) == 0);
    assert(a20_flat_add_fixed(&space, &overlap, (uintptr_t)fixed + 1,
                              sizeof(fixed) - 1) == -EINVAL);
    assert(a20_flat_add_fixed(&space, &overlap, (uintptr_t)-2, 10) == -EINVAL);
    assert(a20_flat_add_fixed(&space, &permanent, (uintptr_t)fixed + 256, 1) ==
           -EINVAL);
    uintptr_t first = 77, second = 0;
    assert(a20_flat_alloc(&space, 0, &first) == -EINVAL && first == 77);
    assert(a20_flat_alloc(&space, (size_t)-1, &first) == -EINVAL &&
           first == 77);
    fail_after = 0;
    assert(a20_flat_alloc(&space, 4096, &first) == -ENOMEM && !outstanding &&
           first == 77);
    fail_after = 1;
    assert(a20_flat_alloc(&space, 4096, &first) == -ENOMEM && !outstanding &&
           first == 77);
    fail_after = -1;
    assert(a20_flat_alloc(&space, 4097, &first) == 0 && !(first & 4095));
    for (unsigned i = 0; i < 8192; i++)
        assert(!((unsigned char *)first)[i]);
    unsigned value = 0xabcdef12, output = 17;
    assert(a20_flat_copy(&space, &value, (void *)(first + 8192 - 4), 4, 1) ==
           0);
    assert(a20_flat_copy(&space, &output, (void *)(first + 8192 - 4), 4, 0) ==
               0 &&
           output == value);
    assert(a20_flat_copy(&space, &output, (void *)(first + 8192 - 3), 4, 0) ==
               -EFAULT &&
           output == value);
    assert(!a20_flat_pin(&space, (uintptr_t)-1, 4));
    assert(!a20_flat_pin(&space, first + 8191, 2));
    a20_flat_region_t *pin = a20_flat_pin(&space, first, 4);
    assert(pin && a20_flat_unmap(&space, first, 4097) == -EBUSY);
    assert(a20_flat_destroy(&space) == -EBUSY);
    assert(a20_flat_unmap(&space, first + 4096, 4096) == -EINVAL);
    a20_flat_unpin(&space, pin);
    assert(a20_flat_unmap(&space, first, 4097) == 0 && !outstanding);
    assert(a20_flat_copy(&space, &output, (void *)first, 4, 0) == -EFAULT);
    assert(a20_flat_unmap(&space, first, 4097) == -EINVAL);
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; i++)
        assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
    for (unsigned i = 0; i < 8; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(!outstanding);
    assert(a20_flat_alloc(&space, 8192, &first) == 0);
    assert(a20_flat_alloc(&space, 4096, &second) == 0 && outstanding == 4);
    pin = a20_flat_pin(&space, (uintptr_t)fixed, sizeof(fixed));
    assert(pin && a20_flat_destroy(&space) == -EBUSY);
    a20_flat_unpin(&space, pin);
    assert(a20_flat_destroy(&space) == 0 && !outstanding);
    assert(a20_flat_alloc(&space, 4096, &first) == -EINVAL && !outstanding);
    assert(!a20_flat_pin(&space, (uintptr_t)fixed, 1));
    assert(a20_flat_copy(&space, &output, fixed, 4, 0) == -EFAULT);
    assert(a20_flat_destroy(&space) == 0);
    puts("PASS flat user: ranges, OOM rollback, pins, close/reclaim, 8 threads "
         "x 200 allocations");
    return 0;
}
