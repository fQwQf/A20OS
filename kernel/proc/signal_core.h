#ifndef _PROC_SIGNAL_CORE_H
#define _PROC_SIGNAL_CORE_H

#include <stdint.h>

/*
 * Rust signal decision core ABI (kernel/proc/signal_core.rs).
 *
 * Rust owns the transplantable signal business logic: disposition
 * classification, status encodings, mask transitions and operand stripping,
 * deliverability/fatality decisions, pending-queue coalescing policy,
 * pending-info scope selection, generation-cancel rules, delivery selection,
 * restart-after-handler policy, action lifecycle helpers, syscall-argument
 * validation shared by the Linux and Native ABIs, the Native checkpoint
 * thread-check transaction, and signalfd readiness/batching decisions.
 *
 * The C adapters keep the IRQ-safe locks, task and signal_state access,
 * usercopy, wait queues, the hardware signal frame and arch assembly.
 *
 * Kernel-mask convention (kernel/include/proc/signal.h): bit position ==
 * signum, bit 0 unused. All masks here are kernel masks.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Default-action classification and status encodings ---- */
int      a20_signal_core_dump_default(int sig);
int      a20_signal_default_terminate(int sig);
int      a20_signal_default_stop(int sig);
int      a20_signal_default_ignore(int sig);
int      a20_signal_wait_status_dumped(int sig, int dumped);
int      a20_signal_fatal_exit_code_dumped(int sig, int dumped);

/* ---- Mask transitions and operand policy ---- */
/* sigprocmask-style transition. An invalid `how` returns `old` unchanged
 * (callers that must reject invalid `how` validate it before calling). */
uint64_t a20_signal_mask_apply(int how, uint64_t old, uint64_t set);
/* Remove SIGKILL/SIGSTOP from a kernel mask. */
uint64_t a20_signal_mask_strip_unblockable(uint64_t mask);
uint64_t a20_signal_deliverable_mask(uint64_t pending, uint64_t thread_pending,
                                     uint64_t blocked);
uint64_t a20_signal_pending_union(uint64_t pending, uint64_t thread_pending);
uint64_t a20_signal_pending_blocked(uint64_t pending, uint64_t thread_pending,
                                    uint64_t blocked);
/* Lowest signal number in a kernel mask, or 0 when empty. */
int      a20_signal_lowest(uint64_t mask);

/* ---- Deliverability and fatality ---- */
int      a20_signal_action_deliverable(uint64_t blocked_mask, uint64_t handler,
                                       int sig);
int      a20_signal_action_fatal(uint64_t blocked_mask, uint64_t handler,
                                 int sig);
int      a20_signal_action_user_handler_available(uint64_t handler, int blocked);

/* ---- Queue coalescing policy ---- */
int      a20_signal_queue_discard(int sig, uint64_t handler);
int      a20_signal_queue_update_info(int thread_directed, int thread_bit_set);
int      a20_signal_queue_info_has(int thread_directed, int thread_bit_set,
                                   int info_present);
/* 0 = thread scope, 1 = process scope, -ENOENT = no info. */
int      a20_signal_peek_scope(int thread_bit, int thread_info_obj,
                               int thread_has_info, int process_has_info);
/* 0 = thread scope, 1 = process scope. */
int      a20_signal_clear_scope(int thread_bit);

/* ---- Generation-cancel rules ---- */
uint64_t a20_signal_generation_cancel_mask(int sig);

/* ---- Restart-after-handler policy ---- */
int      a20_signal_restart_skips(uint64_t handler, int sig);
int      a20_signal_restart_vetoes(uint64_t handler, int flags);

/* ---- Action lifecycle helpers ---- */
int      a20_signal_sigchld_auto_reap(uint64_t handler, int flags);
int      a20_signal_sigchld_no_cldstop(int flags);
int      a20_signal_continue_pending(uint64_t pending_union);
uint64_t a20_signal_temporary_mask(uint64_t new_mask);
int      a20_signal_sigsuspend_restore_active(int sigsuspend_active,
                                              int sig_handling);
uint64_t a20_signal_exec_action_reset(uint64_t handler);

/* ---- Syscall-argument validation ---- */
int      a20_signal_action_valid(int sig, uint64_t sigsetsize,
                                 uint64_t arch_sigset_size);
int      a20_signal_procmask_how_valid(int how);
int      a20_signal_kill_permission(int cap_kill, uint32_t self_uid,
                                    uint32_t self_euid, uint32_t target_uid,
                                    uint32_t target_suid);
int      a20_signal_sigaltstack_valid(uint32_t flags, uint64_t size,
                                      uint64_t min_size);
/* Native helpers return 1 when the argument is invalid, else 0. */
int      a20_signal_native_kill_valid(uint64_t mode, int64_t sig, int nsig);
int      a20_signal_native_mode_valid(uint64_t mode, uint64_t allowed);
int      a20_signal_native_control_valid(uint32_t operation, uint32_t flags,
                                         uint32_t op_check, uint32_t op_mask,
                                         uint64_t check_mask, uint64_t mask_mask);

/* ---- Native checkpoint thread-signal transaction ---- */
uint64_t a20_signal_native_thread_check(uint64_t pending_ht,
                                        uint64_t pending_thread,
                                        uint64_t blocked, uint32_t mode,
                                        uint64_t selected,
                                        uint64_t *clear_thread_out,
                                        uint64_t *clear_ht_out);
uint64_t a20_signal_native_thread_mask(uint64_t new_mask);
uint64_t a20_signal_native_ht_mask(uint64_t new_mask);

/* ---- signalfd readiness / batching ---- */
int      a20_signal_fd_ready(uint64_t pending_union, uint64_t mask);
int      a20_signal_fd_read_valid(uint64_t count, uint64_t record_size);
uint64_t a20_signal_fd_read_records(uint64_t count, uint64_t record_size);

/* ============================================================
 * State transactions (Rust owns the mutation; the C adapter holds the
 * signal_state lock and supplies raw pointers to the process-scope state
 * and the optional per-thread pending-info object)
 * ============================================================ */

/* Borrowed view of the pending/queue state.  The per-thread pair is NULL
 * until the adapter allocates the object. */
typedef struct a20_signal_queue_state {
    uint64_t *process_pending;
    uint8_t  *process_has_info; /* [64] */
    uint8_t  *process_info;     /* [64][128] */
    uint64_t *thread_pending;
    uint8_t  *thread_has_info;  /* [64] or NULL */
    uint8_t  *thread_info;      /* [64][128] or NULL */
} a20_signal_queue_state_t;

/* Peek queued siginfo (thread occurrence preferred); 0, -ENOENT or -EINVAL. */
int  a20_signal_peek_locked(a20_signal_queue_state_t *st, int sig,
                            uint8_t *out, size_t out_size);
/* Consume one occurrence (thread scope preferred); same returns as peek. */
int  a20_signal_consume_locked(a20_signal_queue_state_t *st, int sig,
                               uint8_t *out, size_t out_size);

typedef struct a20_signal_queue_result {
    int discard;       /* signal dropped at generation */
    int fatal;         /* deliverable + SIG_DFL + terminate */
    int deliverable;   /* deliverable after queueing */
    int sigwait_match; /* caller's sigwait set matches this signal */
} a20_signal_queue_result_t;

/* The complete queue transaction: generation-cancel in both scopes, discard
 * decision, scope coalescing + siginfo write, pending bit, side effects. */
int  a20_signal_queue_locked(a20_signal_queue_state_t *st, int signum,
                             const uint8_t *info, size_t info_size,
                             uint64_t handler, uint64_t blocked,
                             int thread_directed, int sigwait_active,
                             uint64_t sigwait_mask,
                             a20_signal_queue_result_t *result);

/* Per-task scalar slots cleared by the exec transaction. */
typedef struct a20_signal_task_slots {
    int      *sig_handling;
    int      *sigsuspend_active;
    int      *sigwait_active;
    uint64_t *sigwait_mask;
} a20_signal_task_slots_t;

/* exec reset: dispositions (reset to SIG_DFL except SIG_IGN, flags/mask
 * cleared), all pending state from both scopes, and the task scalars.
 * handler_off/mask_off/flags_off/handler_size describe the C sigaction_t. */
int  a20_signal_exec_reset_locked(a20_signal_queue_state_t *st,
                                  uint8_t *actions, size_t actions_count,
                                  size_t action_stride, size_t handler_off,
                                  size_t handler_size, size_t mask_off,
                                  size_t flags_off,
                                  a20_signal_task_slots_t *slots);

/* ---- Delivery selection transaction ---- */
enum {
    A20_DELIVERY_NONE = 0,        /* nothing deliverable */
    A20_DELIVERY_DROP_IGNORED = 1,/* consumed an ignored occurrence */
    A20_DELIVERY_STOP = 2,        /* default stop; C stops the task */
    A20_DELIVERY_FATAL = 3,       /* default fatal; C dumps/exits */
    A20_DELIVERY_HANDLER = 4,     /* C builds the handler frame */
    A20_DELIVERY_PTRACE_STOP = 5  /* C enters the ptrace signal-stop */
};

typedef struct a20_signal_delivery_txn {
    uint64_t blocked;                /* in/out */
    int      sigsuspend_active;      /* in/out */
    uint64_t sigsuspend_old_blocked; /* in */
    int      user;                   /* in: user-context delivery */
    int      traced;                 /* in: proc_debug_is_traced */
    int      ptrace_deliver_sig;     /* in/out: one-shot resume marker */
    int      sig;                    /* out */
    int      outcome;                /* out: A20_DELIVERY_* */
    uint64_t handler;                /* out */
    int      flags;                  /* out: sa_flags */
    uint64_t sa_mask;                /* out: sa_mask */
    uint64_t old_blocked;            /* out: mask saved in the frame */
    int      sig_handling;           /* out */
    int      reset_handler;          /* out: C applies the RESETHAND store */
    int      has_info;               /* out: queued info was copied */
} a20_signal_delivery_txn_t;

/* One delivery-loop iteration: select the lowest deliverable signal and
 * perform the entire per-iteration state transition. */
int  a20_signal_delivery_select_locked(a20_signal_queue_state_t *st,
                                       const uint8_t *actions,
                                       size_t actions_count,
                                       size_t action_stride,
                                       size_t handler_off,
                                       size_t handler_size,
                                       size_t mask_off, size_t flags_off,
                                       a20_signal_delivery_txn_t *txn,
                                       uint8_t *info_out, size_t info_out_size);

/* ---- signalfd batch consumption ---- */
/* Dequeue up to `capacity` matching signals in one transaction, one 128-byte
 * signalfd record per consumed occurrence.  `sifields_off` is
 * offsetof(arch_siginfo_t, _sifields) on the target (16 on 64-bit, 12 on
 * 32-bit).  Returns count, 0, or -EINVAL. */
int64_t a20_signal_fd_read_locked(a20_signal_queue_state_t *st, uint64_t mask,
                                  uint8_t *records, size_t capacity,
                                  size_t record_size, size_t sifields_off);

/* sigwait-style take: lowest pending signal matching `mask` (blocked ones
 * included), consumed in one transaction.  Returns sig or 0. */
int  a20_signal_take_matching_locked(a20_signal_queue_state_t *st,
                                     uint64_t mask, uint8_t *out,
                                     size_t out_size, int *has_info_out);

/* ---- Native checkpoint consuming transactions ---- */
uint64_t a20_signal_native_thread_check_locked(uint64_t *pending_ht,
                                               uint64_t *pending_thread,
                                               uint64_t blocked, uint32_t mode,
                                               uint64_t selected);
uint64_t a20_signal_native_ht_take_locked(uint64_t *pending, uint64_t blocked);
void     a20_signal_native_thread_pend_locked(uint64_t *pending_thread, int sig);
void     a20_signal_native_ht_pend_locked(uint64_t *pending_ht, int sig);

#ifdef __cplusplus
}
#endif

#endif /* _PROC_SIGNAL_CORE_H */
