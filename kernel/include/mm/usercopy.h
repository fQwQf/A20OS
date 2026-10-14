#ifndef _MM_USERCOPY_H
#define _MM_USERCOPY_H

#include "core/types.h"

struct task_t;
struct mm_struct;

/* Internal C/Rust boundary for the selected page-at-a-time copy loops.
 * Success from page_access leaves the exact mm->lock held and returns its
 * irq-save token; NEED_FAULT is 1 and returns unlocked; other errors are
 * negative errno values. */
int a20_usercopy_page_access(struct task_t *task, struct mm_struct *mm,
                             uintptr_t user_address, int write,
                             void **kernel_address, size_t *chunk,
                             uint64_t *irq_flags,
                             unsigned *preempt_before);
int a20_usercopy_resolve_fault(struct task_t *task, struct mm_struct *mm,
                               uintptr_t user_address, int write,
                               uint64_t irq_before, unsigned preempt_before);
void a20_usercopy_page_unlock(struct mm_struct *mm, uint64_t irq_flags,
                              unsigned preempt_before);

long a20_usercopy_copy_from_rust(struct task_t *task, struct mm_struct *mm,
                                 void *dst, const void *src, size_t length);
long a20_usercopy_copy_to_rust(struct task_t *task, struct mm_struct *mm,
                               void *dst, const void *src, size_t length);

#endif
