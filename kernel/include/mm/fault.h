#ifndef _MM_FAULT_H
#define _MM_FAULT_H

#include "core/types.h"

struct task_t;
struct mm_struct;
struct mm_seg;
struct vfile;

int handle_cow_fault(struct task_t *t, uint64_t stval);
int handle_demand_fault(struct task_t *t, uint64_t stval);

/* COW breaks served by the lockless slice (mm_cow_from_status).  Plain
 * global, printed in [MM-ASM]: the shutdown audit reads it even though the
 * faulting process has exited and a20_perf collection was never armed. */
extern uint64_t mm_cow_from_status_count;

enum mm_fault_access {
    MM_FAULT_ACCESS_READ = 0,
    MM_FAULT_ACCESS_WRITE,
    MM_FAULT_ACCESS_EXEC,
};

int handle_demand_fault_access(struct task_t *t, uint64_t stval,
                               enum mm_fault_access access);

/*
 * A concurrent fault on another CPU may have installed the PTE after this CPU
 * cached a non-present translation. Validate the completed mapping and flush
 * the stale translation before treating the fault as a protection violation.
 */
int handle_present_page_fault(struct task_t *t, uint64_t stval,
                              enum mm_fault_access access);
int mm_shared_file_fault(struct mm_struct *mm, struct mm_seg *vma,
                         uint64_t page_va, struct vfile *vf);

#endif /* _MM_FAULT_H */
