/* envwrap: run a command inside a fresh capability envelope.
 *
 * Usage: envwrap <cmd> [args...]
 *
 * Creates an install-time policy envelope (FILE+PIPE classes, full file
 * rights, generous op/data budgets, no SOCKET, no expiry), forks, the
 * child enters the envelope and execs the command; the parent reports
 * the exit status.  Used by the exact-execution corpus evaluation to
 * wrap real package install scripts (docs/research/10-evaluation.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYS_a20_envelope_create 902
#define SYS_a20_envelope_enter  903
#define SYS_a20_envelope_stats  905

struct env_policy {
    unsigned int allowed_types;
    unsigned long long rights_by_class[32];
    unsigned long long time_budget_ns;
    unsigned long long op_budget;
    unsigned long long data_budget;
    unsigned int propagation_types;
    unsigned int flags;
};

#define OBJ_FILE 3
#define OBJ_PIPE 6
#define OBJ_EVENT_QUEUE 8
#define OBJ_TIMER 9
#define R_READ  (1ull << 0)
#define R_WRITE (1ull << 1)
#define R_STAT  (1ull << 3)
#define R_SEEK  (1ull << 4)

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: envwrap <cmd> [args...]\n");
        return 2;
    }

    struct env_policy pol;
    memset(&pol, 0, sizeof(pol));
    /* Install-time policy: filesystem, pipes, and intra-process
     * primitives (eventfd/timerfd/signalfd/epoll class objects) --
     * interpreters need these to boot.  No SOCKET: exfiltration dies
     * at creation. */
    pol.allowed_types = (1u << OBJ_FILE) | (1u << OBJ_PIPE) |
                        (1u << OBJ_EVENT_QUEUE) | (1u << OBJ_TIMER);
    pol.rights_by_class[OBJ_FILE] = R_READ | R_WRITE | R_STAT | R_SEEK;
    pol.rights_by_class[OBJ_PIPE] = R_READ | R_WRITE | R_STAT;
    pol.rights_by_class[OBJ_EVENT_QUEUE] = R_READ | R_WRITE | R_STAT;
    pol.rights_by_class[OBJ_TIMER] = R_READ | R_WRITE | R_STAT;
    pol.op_budget = 100000;
    pol.data_budget = 256ull * 1024 * 1024;

    long id = syscall(SYS_a20_envelope_create, &pol, 0L);
    if (id < 0) {
        fprintf(stderr, "ENVWRAP: create failed rc=%ld\n", id);
        return 2;
    }

    pid_t p = fork();
    if (p < 0)
        return 2;
    if (p == 0) {
        if (syscall(SYS_a20_envelope_enter, id) < 0) {
            fprintf(stderr, "ENVWRAP: enter failed\n");
            _exit(2);
        }
        execvp(argv[1], &argv[1]);
        fprintf(stderr, "ENVWRAP: exec %s failed\n", argv[1]);
        _exit(127);
    }
    int st = 0;
    waitpid(p, &st, 0);
    /* Mediator ground truth: how many acquisitions/uses were denied and
     * why.  This is the attribution evidence for the corpus evaluation --
     * a blocked exfil shows up as acquire_deny_type (socket creation). */
    struct {
        unsigned long long acquire_ok, acquire_deny_type,
            acquire_deny_rights, use_ok, use_deny_ops, use_deny_data,
            use_deny_rights, use_deny_expired, bytes_charged;
        int expired, n_shadows;
    } es;
    memset(&es, 0, sizeof(es));
    if (syscall(SYS_a20_envelope_stats, id, &es) == 0)
        fprintf(stderr,
                "ENVWRAP-STATS deny_type=%llu deny_rights=%llu "
                "deny_ops=%llu deny_data=%llu deny_expired=%llu\n",
                es.acquire_deny_type, es.acquire_deny_rights,
                es.use_deny_ops, es.use_deny_data, es.use_deny_expired);
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 2;
}
