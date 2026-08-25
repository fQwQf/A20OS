/*
 * fscompat/mm/mm.h — 用户态 FS 宿主的内存管理垫片。
 * 磁盘文件系统源码仅需 kmalloc 家族（mm/slab.h 真实声明）；
 * 内核的物理帧分配器与页表设施在宿主中不存在。
 */
#ifndef _MM_H
#define _MM_H

#include "core/types.h"
#include "core/defs.h"
#include "mm/slab.h"

#endif /* _MM_H */
