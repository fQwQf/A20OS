#include "mm/objcache.h"

#include "core/string.h"
#include "mm/slab.h"

void obj_cache_init(obj_cache_t *cache, const char *name, size_t obj_size,
                    size_t max_cached)
{
    if (!cache)
        return;
    cache->name = name;
    cache->obj_size = obj_size < sizeof(obj_cache_node_t) ?
                      sizeof(obj_cache_node_t) : obj_size;
    cache->max_cached = max_cached;
    cache->cached = 0;
    cache->allocs = 0;
    cache->frees = 0;
    cache->hits = 0;
    cache->misses = 0;
    cache->free_list = NULL;
    spin_init(&cache->lock);
}

void *obj_cache_alloc(obj_cache_t *cache)
{
    if (!cache || cache->obj_size == 0)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&cache->lock);
    obj_cache_node_t *node = cache->free_list;
    if (node) {
        cache->free_list = node->next;
        cache->cached--;
        cache->allocs++;
        cache->hits++;
        spin_unlock_irqrestore(&cache->lock, flags);
        return node;
    }
    cache->allocs++;
    cache->misses++;
    spin_unlock_irqrestore(&cache->lock, flags);
    return kmalloc(cache->obj_size);
}

void *obj_cache_alloc_zero(obj_cache_t *cache)
{
    void *obj = obj_cache_alloc(cache);
    if (obj)
        memset(obj, 0, cache->obj_size);
    return obj;
}

void obj_cache_free(obj_cache_t *cache, void *obj)
{
    if (!obj)
        return;
    if (!cache) {
        kfree(obj);
        return;
    }

    uint64_t flags = spin_lock_irqsave(&cache->lock);
    cache->frees++;
    if (cache->cached < cache->max_cached) {
        obj_cache_node_t *node = (obj_cache_node_t *)obj;
        node->next = cache->free_list;
        cache->free_list = node;
        cache->cached++;
        spin_unlock_irqrestore(&cache->lock, flags);
        return;
    }
    spin_unlock_irqrestore(&cache->lock, flags);
    kfree(obj);
}

void obj_cache_get_stats(const obj_cache_t *cache, obj_cache_stats_t *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    if (!cache)
        return;
    stats->cached_objects = __atomic_load_n(&cache->cached, __ATOMIC_RELAXED);
    stats->cached_bytes = stats->cached_objects * cache->obj_size;
    stats->allocs = __atomic_load_n(&cache->allocs, __ATOMIC_RELAXED);
    stats->frees = __atomic_load_n(&cache->frees, __ATOMIC_RELAXED);
    stats->hits = __atomic_load_n(&cache->hits, __ATOMIC_RELAXED);
    stats->misses = __atomic_load_n(&cache->misses, __ATOMIC_RELAXED);
}
