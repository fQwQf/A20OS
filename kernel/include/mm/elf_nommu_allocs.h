#ifndef A20_MM_ELF_NOMMU_ALLOCS_H
#define A20_MM_ELF_NOMMU_ALLOCS_H

#include <stddef.h>
#include <stdint.h>

static inline void elf_nommu_allocs_release(void **allocs, size_t *sizes,
                                            uint8_t *types, int *count,
                                            int capacity,
                                            void (*release)(void *))
{
    if (!allocs || !sizes || !types || !count || !release)
        return;
    int n = *count;
    if (n < 0) n = 0;
    if (n > capacity) n = capacity;
    *count = 0;
    for (int i = 0; i < capacity; i++) {
        void *ptr = i < n ? allocs[i] : NULL;
        allocs[i] = NULL;
        sizes[i] = 0;
        types[i] = 0;
        if (ptr) release(ptr);
    }
}

static inline void elf_nommu_allocs_transfer(void **dst, size_t *dst_sizes,
                                             uint8_t *dst_types, int *dst_count,
                                             void **src, size_t *src_sizes,
                                             uint8_t *src_types, int *src_count,
                                             int capacity)
{
    if (!dst || !dst_sizes || !dst_types || !dst_count || !src ||
        !src_sizes || !src_types || !src_count)
        return;
    int n = *src_count;
    if (n < 0) n = 0;
    if (n > capacity) n = capacity;
    for (int i = 0; i < capacity; i++) {
        dst[i] = i < n ? src[i] : NULL;
        dst_sizes[i] = i < n ? src_sizes[i] : 0;
        dst_types[i] = i < n ? src_types[i] : 0;
        src[i] = NULL;
        src_sizes[i] = 0;
        src_types[i] = 0;
    }
    *dst_count = n;
    *src_count = 0;
}

#endif
