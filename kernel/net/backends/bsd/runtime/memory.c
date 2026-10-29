/* Kernel-owned backing for BSD malloc and UMA. No libc mmap, DPDK allocation,
 * shared mappings or invented physical-contiguity guarantees. Allocation never
 * sleeps or reclaims under a BSD lock; M_WAITOK retry is the BSD caller's job. */
#include "memory.h"
/* Private import aliases keep BSD's own panic/memcpy symbols independent. */
#define kmalloc_atomic port_host_alloc
#define kfree port_host_free
#define memcpy port_host_copy
#define memset port_host_zero
#define panic port_host_panic
#include "mm/slab.h"
#include "core/string.h"
#include "core/panic.h"
#include "net/profile.h"
#include "ff_host_interface.h"

#ifndef BSD_MEMORY_LIMIT
#if CONFIG_NET_PROFILE == CONFIG_NET_PROFILE_EMBEDDED
#define BSD_MEMORY_LIMIT (4UL * 1024 * 1024)
#elif CONFIG_NET_PROFILE == CONFIG_NET_PROFILE_SERVER
#define BSD_MEMORY_LIMIT (256UL * 1024 * 1024)
#else
#define BSD_MEMORY_LIMIT (64UL * 1024 * 1024)
#endif
#endif

#define HEADER_MAGIC ((uintptr_t)0x42324440)
#define PAGE_ALIGNMENT 4096UL
#define HEAP_ALIGNMENT 16UL
struct backing {
    void *base;
    size_t length, charge;
    uintptr_t tag;
} __attribute__((aligned(16)));
static size_t used, peak, allocations, failures;

static int reserve(size_t amount)
{
    size_t previous = __atomic_load_n(&used, __ATOMIC_RELAXED);
    for (;;) {
        if (amount > BSD_MEMORY_LIMIT || previous > BSD_MEMORY_LIMIT - amount) {
            __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
            return 0;
        }
        if (__atomic_compare_exchange_n(&used, &previous, previous + amount,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            size_t high = __atomic_load_n(&peak, __ATOMIC_RELAXED);
            while (high < previous + amount &&
                   !__atomic_compare_exchange_n(&peak, &high, previous + amount,
                                                0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
            return 1;
        }
    }
}

static void *allocate(size_t length, size_t alignment, int mapped)
{
    size_t payload = length ? length : 1;
    size_t overhead = sizeof(struct backing) + alignment - 1;
    if (payload > SIZE_MAX - overhead) {
        __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    size_t charge = payload + overhead;
    if (!reserve(charge)) return NULL;
    void *base = kmalloc_atomic(charge);
    if (!base) {
        __atomic_fetch_sub(&used, charge, __ATOMIC_RELEASE);
        __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    uintptr_t address = ((uintptr_t)base + sizeof(struct backing) + alignment - 1)
                        & ~(uintptr_t)(alignment - 1);
    struct backing *header = (struct backing *)address - 1;
    *header = (struct backing){base, length, charge, HEADER_MAGIC | (uintptr_t)mapped};
    __atomic_fetch_add(&allocations, 1, __ATOMIC_RELAXED);
    return (void *)address;
}

static struct backing *owned(void *address, int mapped)
{
    struct backing *header = (struct backing *)address - 1;
    if (header->tag != (HEADER_MAGIC | (uintptr_t)mapped))
        panic("BSD backing ownership mismatch");
    return header;
}

static void release(struct backing *header)
{
    void *base = header->base;
    size_t charge = header->charge;
    header->tag = 0;
    /* Keep the charge until the allocator has really reclaimed the backing. */
    kfree(base);
    __atomic_fetch_sub(&used, charge, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&allocations, 1, __ATOMIC_RELAXED);
}

void *ff_malloc(uint64_t length)
{
    if (length > SIZE_MAX) return NULL;
    return allocate((size_t)length, HEAP_ALIGNMENT, 0);
}
void ff_free(void *address)
{
    if (address) release(owned(address, 0));
}
void *ff_calloc(uint64_t count, uint64_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    void *address = ff_malloc(count * size);
    if (address) memset(address, 0, (size_t)(count * size));
    return address;
}
void *ff_realloc(void *address, uint64_t length)
{
    if (!address) return ff_malloc(length);
    struct backing *header = owned(address, 0);
    if (!length) { release(header); return NULL; }
    void *next = ff_malloc(length);
    if (!next) return NULL; /* Original allocation stays owned and unchanged. */
    memcpy(next, address, header->length < length ? header->length : (size_t)length);
    release(header);
    return next;
}

void *ff_mmap(void *address, uint64_t length, int prot, int flags, int fd, uint64_t offset)
{
    /* kmem_malloc/UMA need zeroed, page-aligned anonymous writable backing.
     * This is the private BSD kernel allocation contract, not Linux sys_mmap:
     * ff_glue.c checks NULL, so failure must never return MAP_FAILED (-1). */
    if (address || !length || length > SIZE_MAX || length % PAGE_ALIGNMENT ||
        prot != (ff_PROT_READ | ff_PROT_WRITE) || fd != -1 || offset ||
        (flags & ~ff_MAP_NOCORE) != (ff_MAP_PRIVATE | ff_MAP_ANON)) return NULL;
    void *result = allocate((size_t)length, PAGE_ALIGNMENT, 1);
    if (result) memset(result, 0, (size_t)length);
    return result;
}
int ff_munmap(void *address, uint64_t length)
{
    if (!address || !length || length > SIZE_MAX) return -1;
    struct backing *header = owned(address, 1);
    if (header->length != length) return -1; /* No partial unmap/release. */
    release(header);
    return 0;
}
void port_memory_snapshot(struct bsd_memory_stats *out)
{
    *out = (struct bsd_memory_stats){
        BSD_MEMORY_LIMIT,
        __atomic_load_n(&used, __ATOMIC_ACQUIRE),
        __atomic_load_n(&peak, __ATOMIC_RELAXED),
        __atomic_load_n(&allocations, __ATOMIC_RELAXED),
        __atomic_load_n(&failures, __ATOMIC_RELAXED)
    };
}
