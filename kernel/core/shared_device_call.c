#ifdef CONFIG_SHARED_DEVICE_CALL
#include "core/shared_device_call.h"
#include "core/cpu.h"
#include "core/errno.h"
#include "core/smp.h"

enum { SLOT_IDLE, SLOT_REQUEST, SLOT_EXECUTING, SLOT_DONE };
struct device_call_slot {
    unsigned state, owner;
    shared_device_handler_t handler;
    uint32_t operation;
    uint64_t argument, result;
    int status;
};
/* IRQ masking pins the caller and excludes same-CPU competing producers.
 * Release/acquire publication owns every non-atomic slot field. */
static struct device_call_slot slots[CONFIG_NR_CPUS];
static unsigned servicing[CONFIG_NR_CPUS];

void shared_device_poll(void) {
    int irq = arch_irqs_enabled();
    arch_local_irq_disable();
    unsigned self = cpu_current_id();
    if (servicing[self])
        goto out;
    servicing[self] = 1;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        struct device_call_slot *slot = &slots[cpu];
        if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != SLOT_REQUEST ||
            __atomic_load_n(&slot->owner, __ATOMIC_RELAXED) != self)
            continue;
        unsigned expected = SLOT_REQUEST;
        if (!__atomic_compare_exchange_n(&slot->state, &expected, SLOT_EXECUTING,
                                         0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            continue;
        /* Recheck after claiming: another endpoint may have completed an
         * older request between the first state/owner reads (an ABA reuse).
         * Only the claimant may inspect the non-atomic request payload. */
        if (__atomic_load_n(&slot->owner, __ATOMIC_RELAXED) != self) {
            __atomic_store_n(&slot->state, SLOT_REQUEST, __ATOMIC_RELEASE);
            continue;
        }
        slot->status = slot->handler(slot->operation, slot->argument, &slot->result);
        __atomic_store_n(&slot->state, SLOT_DONE, __ATOMIC_RELEASE);
    }
    servicing[self] = 0;
out:
    if (irq) arch_local_irq_enable();
}

int shared_device_call(unsigned owner, shared_device_handler_t handler,
                       uint32_t operation, uint64_t argument, uint64_t *result) {
    int irq = arch_irqs_enabled();
    arch_local_irq_disable();
    unsigned self = cpu_current_id();
    int status;
    if (!handler || !result || owner >= CONFIG_NR_CPUS) {
        status = -EINVAL;
        goto out;
    }
    if (servicing[self]) {
        status = -EDEADLK;
        goto out;
    }
    if (owner == self) {
        servicing[self] = 1;
        status = handler(operation, argument, result);
        servicing[self] = 0;
        goto out;
    }
    if (!smp_cpu_is_online(owner)) {
        status = -ENODEV;
        goto out;
    }
    struct device_call_slot *slot = &slots[self];
    if (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != SLOT_IDLE) {
        status = -EBUSY;
        goto out;
    }
    __atomic_store_n(&slot->owner, owner, __ATOMIC_RELAXED);
    slot->handler = handler;
    slot->operation = operation;
    slot->argument = argument;
    slot->result = 0;
    __atomic_store_n(&slot->state, SLOT_REQUEST, __ATOMIC_RELEASE);
    while (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != SLOT_DONE)
        cpu_relax();
    status = slot->status;
    *result = slot->result;
    __atomic_store_n(&slot->state, SLOT_IDLE, __ATOMIC_RELEASE);
out:
    if (irq) arch_local_irq_enable();
    return status;
}
#endif
