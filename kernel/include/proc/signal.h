#ifndef _SIGNAL_H
#define _SIGNAL_H

#include "core/types.h"
#include "core/consts.h"
#include "core/trap.h"

/* ============================================================
 * POSIX Signal Handling
 * ============================================================ */

#define NSIG    64

/* sigaction structure */
typedef struct sigaction {
    uintptr_t sa_handler;   /* signal handler address (SIG_DFL=0, SIG_IGN=1) */
    uint64_t  sa_mask;      /* signals to block during handler */
    int       sa_flags;
} sigaction_t;

#define SIG_DFL  ((uintptr_t)0)
#define SIG_IGN  ((uintptr_t)1)

/* Per-process signal state */
typedef struct signal_state {
    sigaction_t actions[NSIG];
    uint64_t    pending;     /* bitmask of pending signals */
    uint64_t    blocked;     /* signal mask */
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

void signal_deliver(void);
void signal_deliver_user(trap_context_t *ctx);

/* System call handlers */
int  sys_sigaction_impl(int signum, const sigaction_t *act, sigaction_t *oldact);
int  sys_sigprocmask_impl(int how, const uint64_t *set, uint64_t *oldset);

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

#endif /* _SIGNAL_H */
