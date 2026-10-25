#ifndef A20_CLUSTER_ARCH_STATE_H
#define A20_CLUSTER_ARCH_STATE_H
#include "core/arch.h"
#include "proc/proc.h"
/* SSI1 carries an opaque architecture state record. Its size and alignment
 * are part of that wire version; only arch code interprets its bytes. */
#define CLUSTER_ARCH_STATE_SIZE 672
typedef struct {
    uint8_t bytes[CLUSTER_ARCH_STATE_SIZE] __attribute__((aligned(16)));
} cluster_arch_state_t;
void arch_cluster_state_capture(cluster_arch_state_t *, trap_context_t *);
int arch_cluster_state_valid(const cluster_arch_state_t *);
void arch_cluster_state_restore(const cluster_arch_state_t *, task_t *,
                                trap_context_t *, int live);
void arch_cluster_state_user_entry(const cluster_arch_state_t *, uint64_t *ip,
                                   uint64_t *sp, uint64_t *tls);
#endif
