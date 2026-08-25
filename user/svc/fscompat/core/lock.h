/*
 * fscompat/core/lock.h — 用户态 FS 宿主的空实现自旋锁。
 *
 * 服务进程单线程运行，锁操作退化为no-op；仅保留类型与 API 形状，
 * 使内核磁盘文件系统源码可原样编译进用户态（docs/hybrid-kernel/06-user-fs.md）。
 */
#ifndef _LOCK_H
#define _LOCK_H

#include "core/types.h"

typedef struct spinlock {
    int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_init(spinlock_t *l)
{
    l->locked = 0;
}

static inline void spin_lock(spinlock_t *l)
{
    l->locked = 1;
}

static inline void spin_unlock(spinlock_t *l)
{
    l->locked = 0;
}

static inline unsigned long spin_lock_irqsave(spinlock_t *l)
{
    l->locked = 1;
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags)
{
    (void)flags;
    l->locked = 0;
}

#endif /* _LOCK_H */
