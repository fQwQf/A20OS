#ifndef _SIGNAL_H
#define _SIGNAL_H

#include "core/types.h"
#include "core/consts.h"
#include "core/refcount.h"
#include "core/trap.h"

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

void signal_deliver(void);
void signal_deliver_user(trap_context_t *ctx);

/* System call handlers */
int  sys_sigaction_impl(int signum, const void *act, void *oldact, size_t sigsetsize);
int  sys_sigprocmask_impl(int how, const void *set, void *oldset, size_t sigsetsize);

#define SS_ONSTACK  1
#define SS_DISABLE  2

#define MINSIGSTKSZ 2048

typedef struct sigaltstack {
    void   *ss_sp;
    int     ss_flags;
    size_t  ss_size;
} stack_t;

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int _sifields[29];
} __attribute__((aligned(16))) siginfo_t;

#define USER_SIGSET_WORDS (128 / (int)sizeof(uint64_t))

#ifndef ARCH_UCONTEXT_PAD_FIELDS
#define ARCH_UCONTEXT_PAD_FIELDS
#endif

#ifndef ARCH_SIGFRAME_EXTRA_FIELDS
#define ARCH_SIGFRAME_EXTRA_FIELDS
#endif

typedef arch_sigcontext_t sigcontext_t;

typedef struct ucontext {
    uint64_t        uc_flags;
    uintptr_t       uc_link;
    stack_t         uc_stack;
    uint64_t        uc_sigmask[USER_SIGSET_WORDS];
    ARCH_UCONTEXT_PAD_FIELDS
    sigcontext_t    uc_mcontext;
} __attribute__((aligned(16))) ucontext_t;

typedef struct {
    uint64_t    flag;
    ucontext_t  uc;
    siginfo_t   info;
    ARCH_SIGFRAME_EXTRA_FIELDS
    uint32_t    tramp[2];
} __attribute__((aligned(16))) sig_rt_frame_t;

int64_t sys_rt_sigreturn_impl(trap_context_t *ctx);

#endif /* _SIGNAL_H */
