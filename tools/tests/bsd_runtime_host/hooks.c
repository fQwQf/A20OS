#include "hooks.h"
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
struct host_block { size_t size; max_align_t alignment; };
_Atomic size_t backing_bytes, backing_count;
_Atomic int fail_next;
void *port_host_alloc(size_t n)
{
    if (atomic_exchange(&fail_next, 0)) return NULL;
    struct host_block *h = malloc(sizeof(*h) + n);
    if (!h) return NULL;
    h->size = n;
    memset(h + 1, 0xa5, n);
    atomic_fetch_add(&backing_bytes, n);
    atomic_fetch_add(&backing_count, 1);
    return h + 1;
}
void port_host_free(void *p)
{
    struct host_block *h = (struct host_block *)p - 1;
    atomic_fetch_sub(&backing_bytes, h->size);
    atomic_fetch_sub(&backing_count, 1);
    free(h);
}
void *port_host_copy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *port_host_zero(void *d, int c, size_t n) { return memset(d, c, n); }
__attribute__((noreturn)) void port_host_panic(const char *format, ...)
{
    (void)format;
    abort();
}

struct task_t;
static _Thread_local unsigned identity;
static _Thread_local uintptr_t canary;
struct task_t *port_host_current_task(void) { return (struct task_t *)&identity; }
void host_canary_set(uintptr_t value) { canary = value; }
uintptr_t host_canary_get(void) { return canary; }
