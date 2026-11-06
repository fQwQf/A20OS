#ifndef FDTABLE_HOST_TEST_SHIMS_H
#define FDTABLE_HOST_TEST_SHIMS_H

#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES 130
#define MAX_PATH_LEN 64
#define FD_CLOEXEC 1
#define O_CLOEXEC 02000000
#define PROC_WAKE_EVENT 1

typedef struct { volatile int value; } refcount_t;
typedef struct { pthread_mutex_t mutex; struct task_t *owner; } spinlock_t;
typedef struct { spinlock_t lock; void *head; uint64_t generation; } wait_queue_t;

#define SPINLOCK_INIT { PTHREAD_MUTEX_INITIALIZER, NULL }
#define WAIT_QUEUE_INIT { SPINLOCK_INIT, NULL, 0 }

static inline void spin_init(spinlock_t *lock)
{
    pthread_mutex_init(&lock->mutex, NULL);
    lock->owner = NULL;
}
static inline void spin_set_debug(spinlock_t *lock, const char *name, void *container)
{
    (void)lock; (void)name; (void)container;
}
static inline uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    pthread_mutex_lock(&lock->mutex);
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    (void)flags;
    pthread_mutex_unlock(&lock->mutex);
}
static inline void spin_lock(spinlock_t *lock) { pthread_mutex_lock(&lock->mutex); }
static inline void spin_unlock(spinlock_t *lock) { pthread_mutex_unlock(&lock->mutex); }
static inline void refcount_set(refcount_t *ref, int value) { ref->value = value; }
static inline void refcount_inc(refcount_t *ref) { __atomic_add_fetch(&ref->value, 1, __ATOMIC_RELAXED); }
static inline int refcount_inc_not_zero(refcount_t *ref)
{
    int old = __atomic_load_n(&ref->value, __ATOMIC_RELAXED);
    while (old != 0) {
        if (__atomic_compare_exchange_n(&ref->value, &old, old + 1, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}
static inline int refcount_dec_and_test(refcount_t *ref)
{
    return __atomic_sub_fetch(&ref->value, 1, __ATOMIC_ACQ_REL) == 0;
}

typedef struct task_t {
    void *files;
    int pid;
    struct { uint64_t nofile; } limits;
    spinlock_t park_lock;
    struct task_t *next;
} task_t;

struct vfile {
    refcount_t ref_count;
    uint64_t identity;
};
typedef struct vfile vfile_t;

extern spinlock_t tasklist_lock;
task_t *proc_current(void);
task_t *proc_first_task_locked(void);
task_t *proc_next_task_locked(task_t *task);
int proc_task_pid(const void *task);

static inline void wait_queue_init(wait_queue_t *q)
{
    spin_init(&q->lock);
    q->head = NULL;
    q->generation = 0;
}
static inline unsigned wait_queue_wake_all(wait_queue_t *q, uintptr_t key, int reason)
{
    (void)q; (void)key; (void)reason; return 0;
}

void vfile_get(vfile_t *vf);
int vfile_put_ref_only(vfile_t *vf);
void vfs_put_file(vfile_t *vf);
int vfs_finalize_closed_vfile(vfile_t *vf);
void vfs_release_process_file_locks(vfile_t *vf, int pid);
void vfs_task_fs_pins_release(task_t *task);
void env_kind_unregister(int fd);
int env_active(task_t *task);
void mntns_release_task(task_t *task);
void pidns_release_task(task_t *task);
void resource_ns_release_task(task_t *task);
void userns_release_task(task_t *task);
vfile_t *devfs_create_stdio(int fd);
void panic(const char *fmt, ...);
void *kmalloc(size_t size);
void *kmalloc_atomic(size_t size);
void kfree(void *ptr);
uint64_t timer_get_ticks(void);
unsigned x86_64_apic_to_cpu(unsigned apic_id);
void kallsyms_print(uint64_t addr);

#define ktrace_fd(...) ((void)0)

#endif
