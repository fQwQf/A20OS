/*
 * A20OS — syscall-filter extension point (KEP_POINT_SYSCALL_FILTER).
 *
 * Registered at boot; programs attached here run at every syscall entry
 * with a typed context describing the pending syscall:
 *
 *   words[KEP_SCF_NR]    syscall number
 *   words[KEP_SCF_ARG0..5] arguments
 *   words[KEP_SCF_ABI]   0 = Linux ABI, 1 = Native ABI
 *
 * Verdicts: KEP_SCF_ALLOW (0), KEP_SCF_DENY (1, syscall fails with
 * -EACCES), KEP_SCF_KILL (2, calling task is terminated).
 */

#include "ext/kep.h"

#include "proc/proc.h"

static kep_point_t kep_syscall_filter_point = {
    .id = KEP_POINT_SYSCALL_FILTER,
    .name = "syscall-filter",
    .nwords = KEP_SCF_WORDS,
};

void kep_syscall_filter_init(void)
{
    (void)kep_register_point(&kep_syscall_filter_point);
}

/* Called from syscall_dispatch() at syscall entry.  Returns 0 when the
 * syscall may proceed; otherwise the action to take:
 *   1 -> deny with -EACCES, 2 -> kill the caller. */
int kep_syscall_filter_check(uint64_t nr, const uint64_t *args, int abi)
{
    uint64_t words[KEP_SCF_WORDS];
    for (int i = 0; i < KEP_SCF_ARGS; i++)
        words[KEP_SCF_ARG0 + i] = args[i];
    words[KEP_SCF_NR] = nr;
    words[KEP_SCF_ABI] = (uint64_t)abi;

    kep_ctx_t ctx = {
        .words = words,
        .nwords = KEP_SCF_WORDS,
    };
    return (int)kep_point_run(&kep_syscall_filter_point, &ctx);
}
