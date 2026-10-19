#ifndef A20_ABI_LINUX_SYS_POLICY_H
#define A20_ABI_LINUX_SYS_POLICY_H

#include "core/types.h"

/*
 * C/Rust ABI for the prctl(2) and setns(2) policy in
 * kernel/abi/linux/sys_policy.rs.
 *
 * Rust owns the argument bounds, the task-field decisions, the parent-alive
 * rule, the bounding-set capability and bit rules, the seccomp mode dispatch,
 * and the namespace-kind/authority rules for setns.  C keeps the current-task
 * lookup, the mm and seccomp calls, the park-locked field access, the name
 * copy, the usercopy, the procfs namespace getters and the namespace joins.
 */

int64_t a20_prctl(uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int32_t a20_setns_kind_flag(int32_t kind);
int32_t a20_setns_nstype_ok(int32_t kind, int32_t nstype);
int32_t a20_setns_authority(int32_t kind, int32_t capable_in_target,
                            int32_t capable_own);
int64_t a20_getid(int32_t self_ok, int32_t target_ok, int32_t value);
int64_t a20_setpgid(int32_t self_ok, int32_t target_ok, int32_t target_pid,
                    int32_t target_ppid, int32_t target_sid, int32_t self_pid,
                    int32_t self_sid, int32_t is_self, int32_t pgid);
int64_t a20_setsid(int32_t self_ok, int32_t pid, int32_t pgid);
int32_t a20_uts_set_check(size_t len, int32_t capable_in_owner);

struct task_t;

/* Adapter bridges (defined in kernel/abi/linux/sys_proc.c). */
struct task_t *a20_pp_current(void);
int      a20_pp_has_mm(struct task_t *t);
int      a20_pp_mm_dumpable(struct task_t *t);
void     a20_pp_mm_set_dumpable(struct task_t *t, int value);
int      a20_pp_parent_alive(struct task_t *t);
void     a20_pp_set_pdeathsig(struct task_t *t, int sig);
int      a20_pp_get_pdeathsig(struct task_t *t);
int      a20_pp_copy_out_sig(uint64_t addr, int sig);
int      a20_pp_has_cap(struct task_t *t, int cap);
uint64_t a20_pp_cap_bounding(struct task_t *t);
void     a20_pp_cap_drop(struct task_t *t, uint32_t bit);
int      a20_pp_nnps(struct task_t *t);
void     a20_pp_set_nnps(struct task_t *t);
int      a20_pp_thp(struct task_t *t);
void     a20_pp_set_thp(struct task_t *t, int value);
int64_t  a20_pp_seccomp_strict(struct task_t *t);
int64_t  a20_pp_seccomp_filter(struct task_t *t, uint64_t addr);
int64_t  a20_pp_seccomp_mode(struct task_t *t);
void     a20_pp_set_name(struct task_t *t, uint64_t addr);

#endif /* A20_ABI_LINUX_SYS_POLICY_H */
