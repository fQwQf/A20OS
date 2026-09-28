/*
 * fscompat/mm/mm.h — the memory management shim of the user-space FS host.
 * The disk filesystem sources only need the kmalloc family (truly declared in
 * mm/slab.h); the kernel's physical frame allocator and page table facilities
 * do not exist in the host.
 */
#ifndef _MM_H
#define _MM_H

#include "core/types.h"
#include "core/defs.h"
#include "mm/slab.h"

#endif /* _MM_H */
