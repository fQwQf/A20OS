#ifndef _SIGNAL_H
#define _SIGNAL_H

#include "core/types.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/trap.h"
#include <signal_abi.h>

/* ============================================================
 * POSIX Signal Handling
 * ============================================================ */

#define NSIG    64
#define SIGNAL_INFO_SIZE 128

/* sigaction structure */
typedef struct sigaction {
    uintptr_t sa_handler;   /* signal handler address (SIG_DFL=0, SIG_IGN=1) */
    uint64_t  sa_mask;      /* signals to block during handler */
    int       sa_flags;
} sigaction_t;

#define SIG_DFL  ((uintptr_t)0)
#define SIG_IGN  ((uintptr_t)1)

#define SA_NOCLDSTOP  1
#define SA_NOCLDWAIT  2
#define SA_SIGINFO    4
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000

#define SI_USER     0
#define SI_KERNEL   0x80
#define SI_QUEUE    (-1)
#define SI_TKILL    (-6)
#define CLD_EXITED     1
#define CLD_KILLED     2

/* Per-process signal state */
typedef struct signal_state {
    refcount_t refcount;
    /*
     * SIGNAL_STATE_LOCK_CONTRACT: protects shared actions/process pending
     * state and the per-task mask/thread-pending/sigwait fields of every task
     * referencing this object.  When proc_lock is also needed the order is
     * proc_lock -> signal_state.lock.
     */
    spinlock_t lock;
    sigaction_t actions[NSIG];
    uint64_t    pending;     /* bitmask of pending signals */
    uint8_t     pending_has_info[NSIG];
    uint8_t     pending_info[NSIG][SIGNAL_INFO_SIZE];
} signal_state_t;

/*
 * Internal masks use bit position == signum (bit 0 unused) to keep the code
 * readable when iterating `for (sig = 1; sig < NSIG; sig++)`.
 *
 * The user/kernel syscall ABI, however, follows Linux and encodes signal N at
 * bit (N-1). Convert at the syscall boundary.
 */
static inline uint64_t signal_mask_bit(int sig) {
    return (sig > 0 && sig < 64) ? (1ULL << sig) : 0;
}

static inline uint64_t signal_mask_from_user(uint64_t user_mask) {
    return user_mask << 1;
}

static inline uint64_t signal_mask_to_user(uint64_t kernel_mask) {
    return kernel_mask >> 1;
}

/* Initialize signal state for a new process */
void signal_init(signal_state_t *ss);

/* Copy signal state on fork */
void signal_copy(const signal_state_t *src, signal_state_t *dst);

/* Queue a signal to a process */
int  signal_send(int pid, int signum);
int  signal_send_user(int pid, int signum);
int  signal_send_info(int pid, int signum, const void *info, size_t info_size);
int  signal_send_thread(int tid, int signum);
int  signal_send_thread_user(int tid, int signum);
int  signal_task_has_unblocked(void *task);
int  signal_task_has_fatal(void *task);
int  signal_task_should_restart(void *task);
int  signal_task_user_handler_available(void *task, int signum);
int  signal_task_sigchld_auto_reap(void *task);
int  signal_task_sigchld_no_cldstop(void *task);
int  signal_task_continue_pending(void *task);
int  signal_task_set_temporary_mask(void *task, uint64_t new_mask,
                                    uint64_t *old_mask);
void signal_task_restore_mask(void *task, uint64_t old_mask);
void signal_task_defer_mask_restore(void *task, uint64_t old_mask);
void signal_task_restore_sigsuspend(void *task);
void signal_exec_reset(void *task);
uint64_t signal_task_pending_blocked(void *task);

void signal_deliver(void);
void signal_deliver_user(trap_context_t *ctx);

/* System call handlers */
int  sys_sigaction_impl(int signum, const void *act, void *oldact, size_t sigsetsize);
int  sys_sigprocmask_impl(int how, const void *set, void *oldset, size_t sigsetsize);

#define SS_ONSTACK  1
#define SS_DISABLE  2

#define MINSIGSTKSZ 2048

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

int64_t sys_rt_sigreturn_impl(trap_context_t *ctx);

/*
 * Architecture-specific hook called just before a signal frame is copied to
 * user memory.  The generic code has already filled @frame and computed the
 * trampoline address.  Architectures that need to adjust the frame (for
 * example x86_64, where the top-of-stack word is the signal handler's return
 * address) can modify it here.  The default weak implementation is a no-op.
 */
void arch_signal_prepare_frame(arch_sig_rt_frame_t *frame, vaddr_t tramp_addr,
                                trap_context_t *ctx);

/*
 * Architecture-specific hook called once when a new user address space is
 * created (exec).  It can map a per-process signal-trampoline page.  The
 * default weak implementation is a no-op.
 */
struct mm_struct;
void arch_setup_signal_trampoline(struct mm_struct *mm);

#endif /* _SIGNAL_H */
