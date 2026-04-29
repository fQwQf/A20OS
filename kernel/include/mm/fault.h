#ifndef _MM_FAULT_H
#define _MM_FAULT_H

#include "core/types.h"

struct task_t;

int handle_cow_fault(struct task_t *t, uint64_t stval);
int handle_demand_fault(struct task_t *t, uint64_t stval);

#endif /* _MM_FAULT_H */
