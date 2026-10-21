#ifndef A20_PROC_PTRACE_STATE_H
#define A20_PROC_PTRACE_STATE_H

#include "core/types.h"

/*
 * C/Rust ABI for the ptrace stop/resume state machine in
 * kernel/proc/ptrace_state.rs.
 *
 * Rust owns when a stop is published and with which kind and siginfo, the
 * attach/seize rules, interrupt, detach, resume, kill, options, the event
 * message and the tracer-death sweep.  C keeps the tracee lookup and
 * references, the locks, the architecture register/trap-context primitives,
 * the sched park/resume calls, signal delivery, the reparent/wake helpers and
 * the register/address-space accessors.
 */
typedef struct pt_view {
    void **ptracer;
    uint32_t *flags;
    int *stop_active;
    int *stop_kind;
    int *event;
    uint64_t *event_msg;
    int *stop_sig;
    int *siginfo_valid;
    int *deliver_sig;
    int *ctx_valid;
    int *orig_parent_pid;
    int *stop_report_pending;
    int *continue_report_pending;
    int *state;
    int *pid;
    int *ppid;
    void **parent;
    void **trap_ctx;
} pt_view_t;

_Static_assert(sizeof(pt_view_t) == 18 * sizeof(void *),
               "pt_view_t layout must match the Rust PtView");

struct task_t;

int      a20_pt_view_fill(struct task_t *t, pt_view_t *out);
struct task_t *a20_pt_current(void);
struct task_t *a20_pt_idle_task(void);
int      a20_pt_may_access(struct task_t *a, struct task_t *b);
int      a20_pt_is_traced(const struct task_t *t);
struct task_t *a20_pt_get(struct task_t *t);
void     a20_pt_put(struct task_t *t);
uint64_t a20_pt_park_lock(struct task_t *t);
void     a20_pt_park_unlock(struct task_t *t, uint64_t flags);
int      a20_pt_parent_alive(struct task_t *t);
void     a20_pt_save_ctx(struct task_t *t);
int      a20_pt_stop(struct task_t *t, int sig);
void     a20_pt_snapshot_siginfo(struct task_t *t, int sig);
void     a20_pt_default_siginfo(struct task_t *t, int sig, int code);
void     a20_pt_send_stop(struct task_t *t);
void     a20_pt_fold_ctx(struct task_t *t, int kind);
int      a20_pt_resume_stopped(struct task_t *t);
int      a20_pt_signal_send(int pid, int sig);
void     a20_pt_reparent_original(struct task_t *t, int orig_pid);

/* Rust state machine. */
int64_t a20_pt_signal_stop(struct task_t *t, int sig);
int64_t a20_pt_event_stop(struct task_t *t, int sig, int event, uint64_t msg);
int64_t a20_pt_syscall_stop(struct task_t *t, int entry);
int64_t a20_pt_traceme(struct task_t *t);
int64_t a20_pt_attach(struct task_t *t, struct task_t *caller, int seize);
int64_t a20_pt_interrupt(struct task_t *t, struct task_t *caller);
int64_t a20_pt_detach(struct task_t *t, int sig);
int64_t a20_pt_resume(struct task_t *t, int sig, int mode, int step);
int64_t a20_pt_singlestep(struct task_t *t, int sig);
int64_t a20_pt_kill(struct task_t *t, struct task_t *caller);
int64_t a20_pt_setoptions(struct task_t *t, uint64_t options);
int64_t a20_pt_geteventmsg(struct task_t *t);

#define PT_TRACER_EXIT_MAX 64
typedef struct pt_tracer_exit_lists {
    int32_t kill_pids[PT_TRACER_EXIT_MAX];
    int32_t resume_pids[PT_TRACER_EXIT_MAX];
    int32_t tracee_pids[PT_TRACER_EXIT_MAX];
    size_t kill_count;
    size_t resume_count;
    size_t tracee_count;
} pt_tracer_exit_lists_t;

void a20_pt_tracer_exiting(struct task_t *tracer, pt_tracer_exit_lists_t *lists);

#endif /* A20_PROC_PTRACE_STATE_H */
