#ifndef _OBJ_CACHE_H
#define _OBJ_CACHE_H

#include "core/types.h"
#include "core/lock.h"

typedef struct obj_cache_node {
    struct obj_cache_node *next;
} obj_cache_node_t;

typedef struct obj_cache {
    const char       *name;
    size_t            obj_size;
    size_t            max_cached;
    size_t            cached;
    size_t            allocs;
    size_t            frees;
    size_t            hits;
    size_t            misses;
    obj_cache_node_t *free_list;
    spinlock_t        lock;
} obj_cache_t;

typedef struct obj_cache_stats {
    size_t cached_objects;
    size_t cached_bytes;
    size_t allocs;
    size_t frees;
    size_t hits;
    size_t misses;
} obj_cache_stats_t;

#define OBJ_CACHE_INIT(cache_name, type, max_objs) \
    { (cache_name), sizeof(type), (max_objs), 0, 0, 0, 0, 0, NULL, SPINLOCK_INIT }

void  obj_cache_init(obj_cache_t *cache, const char *name, size_t obj_size,
                     size_t max_cached);
void *obj_cache_alloc(obj_cache_t *cache);
void *obj_cache_alloc_zero(obj_cache_t *cache);
void  obj_cache_free(obj_cache_t *cache, void *obj);
void  obj_cache_get_stats(const obj_cache_t *cache, obj_cache_stats_t *stats);

#endif
