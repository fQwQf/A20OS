/*
 * A20OS user-space driver (udriver) support — kernel side.
 *
 * Design reference: docs/hybrid-kernel/00-design.md §3.2 (driver
 * migration model).  The kernel keeps interrupt dispatch and MMIO
 * authorization; device logic moves to user tasks:
 *
 *  - udriver_map_mmio() maps a whitelisted physical device window into a
 *    user address space (PFNMAP).  Only explicitly registered windows are
 *    mappable, so tasks cannot map arbitrary RAM or other devices.
 *  - udriver_irq_listen() binds a physical IRQ to a native event queue:
 *    the kernel thunk masks the line at the irqchip and posts
 *    A20_EVENT_SIGNALED; the user handler acknowledges the device and
 *    re-arms with udriver_irq_ack() (the VFIO/UIO level-IRQ protocol).
 *  - udriver_task_cleanup() releases a dead task's IRQ registrations.
 *
 * TODO: derive the window table from FDT compatible strings at boot
 * instead of the static per-board table below.
 */
#ifndef _DRIVERS_CORE_UDRIVER_H
#define _DRIVERS_CORE_UDRIVER_H

#include "core/types.h"

struct mm_struct;
struct a20_eventq;

int  udriver_map_mmio(struct mm_struct *mm, uint64_t phys, uint64_t size,
                      uint32_t prot, uint64_t *out_va);
int  udriver_irq_listen(uint32_t irq, struct a20_eventq *queue,
                        uint64_t user_data, int owner_pid);
int  udriver_irq_ack(uint32_t irq);
int  udriver_irq_unlisten(uint32_t irq);
void udriver_task_cleanup(int pid);

#endif
