#ifndef _A20_MEM_H
#define _A20_MEM_H

#include "a20_types.h"
#include "a20_syscall.h"

static inline a20_status_t a20_vm_alloc(const a20_vm_alloc_args_t *args)
{
    return a20_syscall6(A20_SYS_vm_alloc, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_vm_unmap(uint64_t addr, uint64_t len)
{
    return a20_syscall6(A20_SYS_vm_unmap, addr, len, 0, 0, 0, 0);
}

static inline a20_status_t a20_vm_protect(uint64_t addr, uint64_t len, uint32_t flags)
{
    return a20_syscall6(A20_SYS_vm_protect, addr, len, flags, 0, 0, 0);
}

static inline a20_status_t a20_vm_alloc_pages(uint64_t num_pages, uint32_t flags,
                                               uint64_t *out_addr)
{
    a20_vm_alloc_args_t args;
    args.size      = sizeof(args);
    args.version   = 1;
    args.addr_hint = 0;
    args.length    = num_pages * 4096;
    args.prot      = flags;
    args.flags     = 0x20;
    args.out_addr  = 0;
    a20_status_t r = a20_vm_alloc(&args);
    if (r >= 0 && out_addr) *out_addr = args.out_addr;
    return r;
}

void *a20_malloc(uint64_t size);
void  a20_free(void *ptr);
void *a20_calloc(uint64_t nmemb, uint64_t size);
void *a20_realloc(void *ptr, uint64_t size);

#endif
