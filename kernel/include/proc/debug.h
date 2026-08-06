#ifndef _PROC_DEBUG_H
#define _PROC_DEBUG_H

/*
 * A20OS kernel-internal debugging interface.
 *
 * ABI-agnostic: this is the kernel's answer to "an external agent observes
 * and controls a task".  The Linux ABI layer maps ptrace(2) request numbers
 * onto these operations; a future Native ABI debugger object would map onto
 * the same surface (register file access, stop/resume protocol, watch
 * events).  No Linux ptrace constants leak in here.
 */

#include "core/types.h"
#include "core/trap.h"
#include "proc/debug_regs.h"

#define PROC_DEBUG_MAX_REGISTERS   32
#define PROC_DEBUG_MAX_FPREGISTERS 64

/* Trace capability flags. */
#define PT_DEBUG_FLAG_TRACED    (1U << 0)  /* task is under observation */
#define PT_DEBUG_FLAG_TRACEME   (1U << 1)  /* tracing began with traceme */
#define PT_DEBUG_FLAG_ATTACHED  (1U << 2)  /* tracing began with attach */
#define PT_DEBUG_FLAG_SYSGOOD   (1U << 3)  /* report syscall stops as SIGTRAP|0x80 */
#define PT_DEBUG_FLAG_TRACEEXEC (1U << 4)  /* report exec as an event stop */
#define PT_DEBUG_FLAG_TRACEEXIT (1U << 5)  /* report exit as an event stop */
#define PT_DEBUG_FLAG_EXITKILL  (1U << 6)  /* kill tracee when tracer dies */
#define PT_DEBUG_FLAG_SYSCALL   (1U << 7)  /* syscall-stop mode active */
#define PT_DEBUG_FLAG_STEP      (1U << 8)  /* one-shot single step pending */

/* Stop kinds. */
enum {
    PT_DEBUG_STOP_NONE = 0,
    PT_DEBUG_STOP_SIGNAL,        /* stopped on a signal */
    PT_DEBUG_STOP_EVENT,         /* stopped on a watch event (exec/exit) */
    PT_DEBUG_STOP_SYSCALL_ENTRY, /* syscall-entry stop */
    PT_DEBUG_STOP_SYSCALL_EXIT,  /* syscall-exit stop */
};

/* Watch events reported through a stop.  Values follow the Linux wait
 * encoding (bits 16..23 of the wait status); keep them in sync with
 * wait.c's reporter. */
#define PT_DEBUG_EVENT_EXEC  4
#define PT_DEBUG_EVENT_EXIT  6

/* Resume modes. */
enum {
    PT_DEBUG_RESUME_CONT = 0,
    PT_DEBUG_RESUME_SYSCALL,
};

/* Register set kinds (Linux NT_* values live in the ABI wrapper). */
#define PT_DEBUG_REGSET_PRSTATUS 1
#define PT_DEBUG_REGSET_FPREGSET 2

/*
 * ---- Operations (tracer context) ----
 */

/* Declare the current task as a tracee of its parent. */
int proc_debug_traceme(void);

/* Attach to pid: reparent to caller and stop it at the next boundary. */
int proc_debug_attach(int pid);

/* Detach from pid; optionally deliver sig when resuming. */
int proc_debug_detach(int pid, int sig);

/* Resume a stopped tracee; mode PT_DEBUG_RESUME_CONT or _SYSCALL. */
int proc_debug_resume(int pid, int sig, int mode);

/* Resume with a one-shot hardware single step (arch permitting). */
int proc_debug_singlestep(int pid, int sig);

/* Kill a tracee (works stopped or running). */
int proc_debug_kill(int pid);

/* Read/write one word of tracee address space (stopped tracee only). */
int proc_debug_peek_word(int pid, uintptr_t addr, long *out);
int proc_debug_poke_word(int pid, uintptr_t addr, long data);

/* Bulk access to tracee address space (stopped tracee only).  Writes break
 * COW pages first so shared frames are never corrupted, and ignore PTE write
 * permission (debugger breakpoint insertion).  Returns bytes copied or a
 * negative errno; a partially copied range stops at the first fault. */
long proc_debug_read(int pid, uintptr_t addr, void *buf, size_t len);
long proc_debug_write(int pid, uintptr_t addr, const void *buf, size_t len);

/* Register file access (stopped tracee only). */
int proc_debug_getregs(int pid, proc_debug_regs_t *out);
int proc_debug_setregs(int pid, const proc_debug_regs_t *in);

/* Raw siginfo snapshot of the current stop. */
int proc_debug_getsiginfo(int pid, void *out, size_t size);
int proc_debug_setsiginfo(int pid, const void *in, size_t size);

/* Options: PT_DEBUG_FLAG_* mask. */
int proc_debug_setoptions(int pid, unsigned long options);

/* Message associated with the current event stop. */
int proc_debug_geteventmsg(int pid, long *out);

/* Register set access (Linux NT_PRSTATUS etc.). */
int proc_debug_getregset(int pid, int kind, void *out, size_t *size);
int proc_debug_setregset(int pid, int kind, const void *in, size_t size);

/*
 * ---- Hooks (tracee context; call only when traced) ----
 */

/* Stop at a syscall boundary; on resume from an entry stop the syscall is
 * re-executed (arch rewinds the saved EPC). */
void proc_debug_syscall_entry(trap_context_t *ctx);
void proc_debug_syscall_exit(trap_context_t *ctx);

/* Called from the exit/reparent path when `tracer` is dying: detach all its
 * tracees, optionally killing them (EXITKILL). */
void proc_debug_tracer_exiting(task_t *tracer);

/* Stop the current task at a signal boundary (called from the signal
 * delivery path).  On resume the signal was either suppressed (sig == 0
 * path) or re-queued by the tracer. */
int  proc_debug_signal_stop(int sig);

/* Stop the current task for a watch event (exec/exit). */
int  proc_debug_event_stop(int sig, int event, uint64_t msg);

/* Queries used by core paths. */
static inline int proc_debug_is_traced(const task_t *t)
{
    return t && (t->ptrace_flags & PT_DEBUG_FLAG_TRACED);
}

static inline int proc_debug_is_in_ptrace_stop(const task_t *t)
{
    return t && t->ptrace_stop_active;
}

#endif /* _PROC_DEBUG_H */
