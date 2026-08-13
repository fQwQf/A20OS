/*
 * A20OS Native SDK — Pager, monitor, task memory, VM region sharing.
 * Design reference: docs/native-abi/09-native-abi-deepening.md
 */
#ifndef _A20_PAGER_H
#define _A20_PAGER_H

#include <stdint.h>
#include "a20_types.h"
#include "a20_syscall.h"

/* ===== Pager (0x0D00) ===== */

/* Create a pager; on success installs a PAGER handle in *out_pager and the
 * page-request channel endpoint in *out_requests. */
static inline a20_status_t a20_pager_create(uint32_t flags,
                                            a20_handle_t *out_pager,
                                            a20_handle_t *out_requests)
{
    a20_pager_create_args_t args;
    args.size        = sizeof(args);
    args.version     = 1;
    args.flags       = flags;
    args.reserved    = 0;
    args.out_pager   = A20_HANDLE_NULL;
    args.out_requests = A20_HANDLE_NULL;
    a20_status_t r = a20_syscall6(A20_SYS_pager_create, (uint64_t)&args,
                                  0, 0, 0, 0, 0);
    if (r == A20_OK) {
        if (out_pager) *out_pager = args.out_pager;
        if (out_requests) *out_requests = args.out_requests;
    }
    return r;
}

/* Attach a PAGED VMO (from a20_vm_create_object) to a pager. */
static inline a20_status_t a20_pager_vmo_attach(a20_handle_t pager,
                                                a20_handle_t vmo)
{
    a20_pager_vmo_args_t args;
    args.size    = sizeof(args);
    args.version = 1;
    args.pager   = pager;
    args.vmo     = vmo;
    return a20_syscall6(A20_SYS_pager_vmo_attach, (uint64_t)&args, 0, 0, 0, 0, 0);
}

/* Supply pages from source VMO into the paged VMO. */
static inline a20_status_t a20_pager_supply_pages(a20_handle_t pager,
                                                  a20_handle_t vmo,
                                                  a20_handle_t source,
                                                  uint64_t vmo_offset,
                                                  uint64_t source_offset,
                                                  uint64_t len,
                                                  uint64_t *out_supplied)
{
    a20_pager_supply_args_t args;
    args.size          = sizeof(args);
    args.version       = 1;
    args.pager         = pager;
    args.vmo           = vmo;
    args.source        = source;
    args.vmo_offset    = vmo_offset;
    args.source_offset = source_offset;
    args.len           = len;
    args.out_supplied  = 0;
    a20_status_t r = a20_syscall6(A20_SYS_pager_supply_pages,
                                  (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r >= 0 && out_supplied) *out_supplied = args.out_supplied;
    return r;
}

/* ===== Monitor (0x0D10) ===== */

/* Create a counter object; on success installs a MONITOR handle. */
static inline a20_status_t a20_monitor_create(a20_handle_t target,
                                              uint32_t kind,
                                              uint64_t period_ns,
                                              a20_handle_t *out_monitor)
{
    a20_monitor_create_args_t args;
    args.size         = sizeof(args);
    args.version      = 1;
    args.target       = target;
    args.kind         = kind;
    args.flags        = 0;
    args.queue        = A20_HANDLE_NULL;
    args.period_ns    = period_ns;
    args.out_monitor  = A20_HANDLE_NULL;
    a20_status_t r = a20_syscall6(A20_SYS_monitor_create, (uint64_t)&args,
                                  0, 0, 0, 0, 0);
    if (r == A20_OK && out_monitor) *out_monitor = args.out_monitor;
    return r;
}

/* Read the current counter value. */
static inline a20_status_t a20_monitor_query(a20_handle_t monitor,
                                             a20_monitor_value_t *out)
{
    return a20_syscall6(A20_SYS_monitor_query, monitor, (uint64_t)out,
                        0, 0, 0, 0);
}

/* ===== task_mem (0x0211 / 0x0212) ===== */

/* Copy between local and remote (target task) iovec pairs.  read=1 reads the
 * target's memory into the local buffers; read=0 writes to the target. */
static inline a20_status_t a20_task_mem(a20_handle_t task, int read,
                                        const a20_iovec_t *local,
                                        uint32_t local_count,
                                        const a20_iovec_t *remote,
                                        uint32_t remote_count,
                                        uint64_t *out_transferred)
{
    a20_task_mem_args_t args;
    args.size             = sizeof(args);
    args.version          = 1;
    args.task             = task;
    args.flags            = 0;
    args.local_iov        = (uint64_t)local;
    args.local_iov_count  = local_count;
    args._pad             = 0;
    args.remote_iov       = (uint64_t)remote;
    args.remote_iov_count = remote_count;
    args._pad2            = 0;
    args.out_transferred  = 0;
    uint64_t nr = read ? A20_SYS_task_mem_read : A20_SYS_task_mem_write;
    a20_status_t r = a20_syscall6(nr, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r >= 0 && out_transferred) *out_transferred = args.out_transferred;
    return r;
}

/* ===== vm_share_region (0x030A) ===== */

/* Export the address range [addr, addr+length) as a new MEMORY handle. */
static inline a20_status_t a20_vm_share_region(uint64_t addr, uint64_t length,
                                               a20_rights_t rights,
                                               a20_handle_t *out_handle)
{
    a20_vm_share_args_t args;
    args.size      = sizeof(args);
    args.version   = 1;
    args.addr      = addr;
    args.length    = length;
    args.rights    = rights;
    args.out_handle = A20_HANDLE_NULL;
    a20_status_t r = a20_syscall6(A20_SYS_vm_share_region, (uint64_t)&args,
                                  0, 0, 0, 0, 0);
    if (r == A20_OK && out_handle) *out_handle = args.out_handle;
    return r;
}

#endif /* _A20_PAGER_H */
