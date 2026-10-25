#ifndef _FRAMEBUFFER_H
#define _FRAMEBUFFER_H
#include "core/types.h"
#include "uapi/a20/framebuffer.h"

/*
 * Linux mmap(2) backend for /dev/fb0.  Maps fb_phys + off into the current
 * address space (PFNMAP, device memory attributes).  Returns the mapped
 * virtual address or a negative errno.  On NOMMU returns fb_phys + off.
 */
int64_t fbdev_linux_mmap(uint64_t addr, size_t len, int prot, int flags,
                         uint64_t off);


#endif
