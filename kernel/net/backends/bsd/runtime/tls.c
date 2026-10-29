/* Clang's emulated-TLS ABI for a bounded, task-owned BSD execution context.
 * The compiler routes every __thread access here; no FS register is touched.
 * A descriptor is immutable and shared. Its storage is private to each owner. */
#include "tls.h"
#include "core/errno.h"
#define proc_current port_host_current_task
#define panic port_host_panic
#define memcpy port_host_copy
#include "proc/proc.h"
#include "core/panic.h"
#include "core/string.h"
#include "ff_host_interface.h"

#define CONTEXTS 8
#define VARIABLES 32
#define ARENA_BYTES 16384
struct emutls_control {
    uintptr_t size, alignment, index;
    const void *initial;
};
struct tls_entry { const struct emutls_control *control; void *address; };
struct port_tls_context {
    uintptr_t owner;
    size_t used, entries;
    struct tls_entry values[VARIABLES];
    uint8_t arena[ARENA_BYTES];
};
struct binding { uintptr_t owner; struct port_tls_context *context; };
static struct binding bindings[CONTEXTS];
#define CLAIMED ((uintptr_t)-1)

static uintptr_t owner(void) { return (uintptr_t)proc_current(); }
struct port_tls_context *port_tls_create(void)
{
    return ff_calloc(1, sizeof(struct port_tls_context));
}
int port_tls_bind(struct port_tls_context *context)
{
    uintptr_t task = owner();
    if (!context || !task || task == CLAIMED) return -EINVAL;
    for (unsigned i = 0; i < CONTEXTS; ++i)
        if (__atomic_load_n(&bindings[i].owner, __ATOMIC_ACQUIRE) == task) return -EBUSY;
    uintptr_t vacant = 0;
    if (!__atomic_compare_exchange_n(&context->owner, &vacant, task, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return -EBUSY;
    for (unsigned i = 0; i < CONTEXTS; ++i) {
        vacant = 0;
        if (!__atomic_compare_exchange_n(&bindings[i].owner, &vacant, CLAIMED, 0,
                                         __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        bindings[i].context = context;
        __atomic_store_n(&bindings[i].owner, task, __ATOMIC_RELEASE);
        return 0;
    }
    __atomic_store_n(&context->owner, 0, __ATOMIC_RELEASE);
    return -ENOSPC;
}
int port_tls_unbind(struct port_tls_context *context)
{
    uintptr_t task = owner();
    if (!context || !task || __atomic_load_n(&context->owner, __ATOMIC_ACQUIRE) != task) return -EINVAL;
    for (unsigned i = 0; i < CONTEXTS; ++i) {
        if (__atomic_load_n(&bindings[i].owner, __ATOMIC_ACQUIRE) != task) continue;
        if (bindings[i].context != context) return -EINVAL;
        bindings[i].context = NULL;
        __atomic_store_n(&bindings[i].owner, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&context->owner, 0, __ATOMIC_RELEASE);
        return 0;
    }
    return -EINVAL;
}
int port_tls_destroy(struct port_tls_context *context)
{
    if (!context) return 0;
    uintptr_t vacant = 0;
    if (!__atomic_compare_exchange_n(&context->owner, &vacant, CLAIMED, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return -EBUSY;
    ff_free(context);
    return 0;
}
void *__emutls_get_address(struct emutls_control *control)
{
    uintptr_t task = owner();
    struct port_tls_context *context = NULL;
    if (task && task != CLAIMED)
        for (unsigned i = 0; i < CONTEXTS; ++i)
            if (__atomic_load_n(&bindings[i].owner, __ATOMIC_ACQUIRE) == task) {
                context = bindings[i].context; break;
            }
    if (!context) panic("BSD TLS access outside its owning kernel task");
    for (size_t i = 0; i < context->entries; ++i)
        if (context->values[i].control == control) return context->values[i].address;
    size_t alignment = control->alignment;
    if (!alignment || alignment > 4096 || (alignment & (alignment - 1)) ||
        !control->size || control->size > ARENA_BYTES || context->entries == VARIABLES)
        panic("BSD TLS descriptor exceeds runtime limits");
    uintptr_t base = (uintptr_t)context->arena;
    size_t offset = ((base + context->used + alignment - 1) & ~(uintptr_t)(alignment - 1)) - base;
    if (offset > ARENA_BYTES || control->size > ARENA_BYTES - offset)
        panic("BSD TLS arena exhausted");
    void *result = context->arena + offset;
    if (control->initial) memcpy(result, control->initial, control->size);
    context->used = offset + control->size;
    context->values[context->entries++] = (struct tls_entry){control, result};
    return result;
}
