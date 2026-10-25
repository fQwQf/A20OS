#ifndef A20_CLUSTER_SSI_INTERNAL_H
#define A20_CLUSTER_SSI_INTERNAL_H
#include "core/arch.h"
#include "proc/proc.h"
#ifdef CONFIG_CLUSTER_SSI
int64_t cluster_ssi_configure(uint64_t address);
int cluster_ssi_intercept(trap_context_t *ctx);
void cluster_ssi_before_user(trap_context_t *ctx);
void cluster_ssi_exit(task_t *t);
int cluster_ssi_task_allowed(const task_t *t);
int cluster_ssi_managed(const task_t *t);
int cluster_ssi_mm_busy(const task_t *t);
#else
static inline int cluster_ssi_intercept(trap_context_t *ctx) {
    (void)ctx;
    return 0;
}
static inline void cluster_ssi_before_user(trap_context_t *ctx) {
    (void)ctx;
}
static inline void cluster_ssi_exit(task_t *t) {
    (void)t;
}
static inline int cluster_ssi_task_allowed(const task_t *t) {
    (void)t;
    return 1;
}
static inline int cluster_ssi_managed(const task_t *t) {
    (void)t;
    return 0;
}
static inline int cluster_ssi_mm_busy(const task_t *t) {
    (void)t;
    return 0;
}
#endif
#endif
