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

#endif
