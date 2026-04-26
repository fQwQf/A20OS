#ifndef _SLAB_H
#define _SLAB_H

#include "core/types.h"

void  slab_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void *kcalloc(size_t nmemb, size_t size);

#endif
