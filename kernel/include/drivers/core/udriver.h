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
struct vmo;

#define UDRIVER_DEVICE_F_IOMMU  (1u << 0)
#define UDRIVER_DEVICE_F_BLOCKED (1u << 1)

typedef struct udriver_device_info {
    uint32_t flags;
    uint16_t devid;
    uint16_t irq;
    uint64_t mmio_base;
    uint64_t mmio_size;
    uint64_t fault_count;
    uint32_t fault_cause;
    uint64_t fault_iova;
} udriver_device_info_t;

int  udriver_map_mmio(struct mm_struct *mm, uint64_t phys, uint64_t size,
                      uint32_t prot, uint64_t *out_va);
void udriver_revoke_mmio(struct mm_struct *mm, uint64_t phys);
int  udriver_mmio_user_owned(uint64_t phys);
/* Whether a user-claimable window physically has a device behind it.
 * virtio-mmio windows are probed by their magic register; declared board
 * devices (goldfish RTC) count as present.  Used by the driver manager to
 * decide whether to activate a user-service driver. */
int  udriver_window_present(uint64_t phys);
int  udriver_match_present(uint32_t bus, uint32_t vendor, uint32_t device);
int  udriver_device_get_info(uint32_t bus, uint32_t vendor, uint32_t device,
                             uint32_t index, int pid,
                             udriver_device_info_t *out);
int  udriver_dma_map(struct vmo *vmo, int pid, uint64_t *out_iova);
int  udriver_dma_address(struct vmo *vmo, int pid, uint32_t page,
                         uint64_t *out_addr);
int  udriver_dma_unmap(struct vmo *vmo, int pid);
int  udriver_claim(uint64_t phys, int pid);
int  udriver_release(uint64_t phys, int pid);
int  udriver_claim_owner(uint64_t phys);
int  udriver_irq_listen(uint32_t irq, struct a20_eventq *queue,
                        uint64_t user_data, int owner_pid);
int  udriver_irq_ack(uint32_t irq);
int  udriver_irq_unlisten(uint32_t irq);
void udriver_task_cleanup(int pid);

#endif
