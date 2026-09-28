/*
 * fscompat/mm/slab.h — declarations for the kmalloc family of the user-space FS
 * host.  Signatures match the kernel version; the implementation is in
 * fscompat/compat.c (on top of malloc).
 * Additionally pulls in core/defs.h: some FS translation units obtain macros
 * such as offsetof through it (in the kernel build that transitive path is
 * provided by mm/mm.h).
 */
#ifndef _SLAB_H
#define _SLAB_H

#include "core/types.h"
#include "core/defs.h"

void *kmalloc(size_t size);
void *kmalloc_atomic(size_t size);
void *kcalloc(size_t nmemb, size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);

#endif /* _SLAB_H */
