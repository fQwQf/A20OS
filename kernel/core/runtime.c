#include "proc/proc.h"
#ifdef A20_TASK_OBJECTS_ENABLED
#include "core/runtime.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/string.h"
static spinlock_t runtime_lock = SPINLOCK_INIT;
static a20_runtime_binding_t runtime_binding;
static unsigned runtime_bound;
int a20_runtime_bind(const a20_runtime_binding_t *binding) {
    if (!a20_runtime_binding_valid(binding))
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&runtime_lock);
    int result = 0;
    if (runtime_bound) {
        if (memcmp(binding, &runtime_binding, sizeof(*binding)))
            result = -EBUSY;
    } else {
        runtime_binding = *binding;
        __atomic_store_n(&runtime_bound, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&runtime_lock, flags);
    return result;
}
void a20_runtime_snapshot(a20_runtime_info_t *out) {
    *out = (a20_runtime_info_t){.size = A20_RUNTIME_BYTES,
                                .version = A20_RUNTIME_VERSION};
    /* Acquire observes the once-published immutable binding. */
    if (!__atomic_load_n(&runtime_bound, __ATOMIC_ACQUIRE))
        return;
    out->state = A20_RUNTIME_BOUND;
    out->boot_epoch = runtime_binding.boot_epoch;
    memcpy(out->instance_id, runtime_binding.instance_id, 16);
    memcpy(out->domain_id, runtime_binding.domain_id, 16);
}
int a20_runtime_object_namespace(a20_object_id_t *out) {
    a20_runtime_info_t info;
    a20_runtime_snapshot(&info);
    if (info.state != A20_RUNTIME_BOUND)
        return 0;
    memcpy(out->instance_id, info.instance_id, 16);
    memcpy(out->domain_id, info.domain_id, 16);
    out->boot_epoch = info.boot_epoch;
    return 1;
}
#endif
