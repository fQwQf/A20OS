/*
 * A20OS Native SDK — user-space driver support.
 *
 * Maps whitelisted device MMIO windows and delivers device IRQs to an
 * event queue (docs/hybrid-kernel/00-design.md §3.2).
 */
#ifndef _A20_DEVICE_H
#define _A20_DEVICE_H

#include "a20_types.h"
#include "a20_syscall.h"

static inline a20_status_t a20_device_map_mmio(uint64_t phys_base,
                                               uint64_t length,
                                               uint32_t prot,
                                               uint64_t *out_addr)
{
    a20_device_map_mmio_args_t args;
    args.size      = sizeof(args);
    args.version   = 1;
    args.prot      = prot;
    args._pad      = 0;
    args.phys_base = phys_base;
    args.length    = length;
    args.out_addr  = 0;

    a20_status_t r = a20_syscall6(A20_SYS_device_map_mmio,
                                  (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_addr)
        *out_addr = args.out_addr;
    return r;
}

static inline a20_status_t a20_device_irq_listen(uint32_t irq,
                                                 a20_handle_t queue,
                                                 uint64_t user_data)
{
    a20_device_irq_listen_args_t args;
    args.size      = sizeof(args);
    args.version   = 1;
    args.irq       = irq;
    args.queue     = queue;
    args.user_data = user_data;
    return a20_syscall6(A20_SYS_device_irq_listen,
                        (uint64_t)&args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_device_irq_ack(uint32_t irq)
{
    return a20_syscall6(A20_SYS_device_irq_ack, irq, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_device_irq_unlisten(uint32_t irq)
{
    return a20_syscall6(A20_SYS_device_irq_unlisten, irq, 0, 0, 0, 0, 0);
}

/* DMA contract (M4): materialize a VMO, then ask the kernel for its
 * physical addresses.  Returns page count (>= 0) or a negative error. */
static inline a20_status_t a20_device_vmo_phys(a20_handle_t vmo,
                                               uint64_t *out_paddrs,
                                               uint32_t max_pages,
                                               uint32_t *out_count)
{
    a20_device_vmo_phys_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.vmo        = vmo;
    args._pad       = 0;
    args.out_paddrs = (uint64_t)out_paddrs;
    args.max_pages  = max_pages;
    args.out_count  = 0;
    a20_status_t r = a20_syscall6(A20_SYS_device_vmo_phys,
                                  (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_count)
        *out_count = args.out_count;
    return r;
}

/* Attach a one-page ring VMO to the kernel block proxy (M4).  Returns the
 * doorbell channel endpoint handle via *out_doorbell. */
static inline a20_status_t a20_device_block_attach(a20_handle_t ring_vmo,
                                                   uint64_t capacity,
                                                   a20_handle_t *out_doorbell)
{
    a20_device_block_attach_args_t args;
    args.size         = sizeof(args);
    args.version      = 1;
    args.ring_vmo     = ring_vmo;
    args._pad         = 0;
    args.capacity     = capacity;
    args.out_doorbell = 0;
    a20_status_t r = a20_syscall6(A20_SYS_device_block_attach,
                                  (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r) && out_doorbell)
        *out_doorbell = (a20_handle_t)args.out_doorbell;
    return r;
}

static inline a20_status_t a20_device_block_complete(uint32_t n_done)
{
    return a20_syscall6(A20_SYS_device_block_complete, n_done, 0, 0, 0, 0, 0);
}

#endif
