#ifndef A20_PROC_TASK_CONSTRUCTION_H
#define A20_PROC_TASK_CONSTRUCTION_H

#include "core/types.h"

struct task_construction_ops;
typedef int (*task_construction_stage_fn)(void *context);
typedef void (*task_construction_wait_fn)(void *context);
typedef void (*task_construction_abort_fn)(void *context,
                                           uint8_t publication_attempted);

enum task_construction_stage {
    TASK_CONSTRUCTION_STAGE_MM = 9,
};

typedef struct task_construction_ops {
    void *context;
    task_construction_stage_fn validate;
    task_construction_stage_fn allocate;
    task_construction_stage_fn base;
    task_construction_stage_fn seccomp;
    task_construction_stage_fn fs_cgroup;
    task_construction_stage_fn files;
    task_construction_stage_fn signals;
    task_construction_stage_fn namespaces;
    task_construction_stage_fn capabilities;
    task_construction_stage_fn mm;
    task_construction_stage_fn arch;
    task_construction_stage_fn publish;
    task_construction_stage_fn post_publish;
    task_construction_stage_fn ready;
    task_construction_wait_fn vfork_wait;
    task_construction_abort_fn abort;
    uint8_t deferred;
    uint8_t vfork;
} task_construction_ops_t;

#ifdef CONFIG_TASK_CONSTRUCTION_TEST
/* One-shot, current-parent-only failure after a real stage has acquired its
 * resources. Exposed only by the test kernel profile. */
int proc_task_construction_test_arm(void *owner, unsigned stage);
int proc_task_construction_test_fail_after(void *owner, unsigned stage);
void proc_task_construction_test_cancel(void *owner);
#endif

#endif
