#ifndef _PROC_TASK_OBJECT_H
#define _PROC_TASK_OBJECT_H
#include "proc/proc.h"
#ifdef A20_TASK_OBJECTS_ENABLED
void a20_task_object_ref(void *object);
void a20_task_object_put(void *object);
/* Caller holds an object reference. Returned task needs proc_put(). */
task_t *a20_task_object_get_task(void *object);
void a20_task_object_detach(task_t *task);
void a20_task_object_task_release(task_t *task);
void a20_task_object_notify(task_t *task, uint32_t event, uint64_t data0,
                            uint64_t data1, int closing);
void proc_pid_final_release(task_t *task);
#else
static inline void a20_task_object_detach(task_t *task) {
    (void)task;
}
static inline void a20_task_object_task_release(task_t *task) {
    (void)task;
}
static inline void proc_pid_final_release(task_t *task) {
    (void)task;
}
static inline void a20_task_object_notify(task_t *task, uint32_t event,
                                          uint64_t data0, uint64_t data1,
                                          int closing) {
    (void)task;
    (void)event;
    (void)data0;
    (void)data1;
    (void)closing;
}
#endif
#endif
