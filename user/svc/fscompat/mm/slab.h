/*
 * fscompat/mm/slab.h — 用户态 FS 宿主的 kmalloc 家族声明。
 * 与内核版签名一致；实现位于 fscompat/compat.c（malloc 之上）。
 * 额外引入 core/defs.h：部分 FS 翻译单元经此获得 offsetof 等宏
 * （内核构建中该传递路径由 mm/mm.h 提供）。
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
