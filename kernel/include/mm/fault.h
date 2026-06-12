#ifndef _MM_FAULT_H
#define _MM_FAULT_H

#include "core/types.h"

struct task_t;
struct mm_struct;
struct vm_area;
struct vfile;

int handle_cow_fault(struct task_t *t, uint64_t stval);
int handle_demand_fault(struct task_t *t, uint64_t stval);
int mm_shared_file_fault(struct mm_struct *mm, struct vm_area *vma,
                         uint64_t page_va, struct vfile *vf);

#endif /* _MM_FAULT_H */
