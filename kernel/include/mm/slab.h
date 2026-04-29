#ifndef _SLAB_H
#define _SLAB_H

#include "core/types.h"

void  slab_init(void);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void *kcalloc(size_t nmemb, size_t size);

typedef struct slab_stats {
    size_t total_pages;
    size_t active_pages;
    size_t spare_pages;
    size_t allocated_objects;
    size_t allocated_bytes;
    size_t total_bytes;
    size_t reclaimable_bytes;
} slab_stats_t;

void slab_get_stats(slab_stats_t *stats);

#endif
