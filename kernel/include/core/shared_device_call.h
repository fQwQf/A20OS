#ifndef A20_SHARED_DEVICE_CALL_H
#define A20_SHARED_DEVICE_CALL_H
#include "core/types.h"

/* Shared execution domain only: same kernel image and pointer ABI. Handler
 * executes on owner with IRQs masked; it must be a bounded leaf operation:
 * no locks, allocation, waiting, printing, scheduling or nested device call.
 * Callers retain their existing device locks; poll may run inside a spin
 * wait, so it must never acquire one of those locks on their behalf. */
typedef int (*shared_device_handler_t)(uint32_t operation, uint64_t argument,
                                       uint64_t *result);
#ifdef CONFIG_SHARED_DEVICE_CALL
int shared_device_call(unsigned owner, shared_device_handler_t handler,
                       uint32_t operation, uint64_t argument, uint64_t *result);
void shared_device_poll(void);
#endif
#endif
