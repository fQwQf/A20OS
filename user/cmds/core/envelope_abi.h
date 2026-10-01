/*
 * Capability envelope user-side ABI -- single shared copy.
 *
 * Values mirror kernel/include/ipc/envelope.h (struct a20_env_policy) and the
 * kernel/include/ipc/ipc.h object/right bits; the syscall numbers come from
 * kernel/abi/linux/syscall_nr.h.  These used to be hand-copied into every
 * envelope_* command, which was an ABI-drift hazard with no enforcement.
 */
#ifndef A20_CMDS_ENVELOPE_ABI_H
#define A20_CMDS_ENVELOPE_ABI_H

#define SYS_a20_envelope_create 902
#define SYS_a20_envelope_enter  903
#define SYS_a20_envelope_revoke 904
#define SYS_a20_envelope_stats  905
#define SYS_a20_envelope_audit  906

/* Must match kernel/include/ipc/ipc.h object types and right bits. */
#define ENV_OBJ_FILE    3
#define ENV_OBJ_SOCKET  5
#define ENV_OBJ_EVENT_QUEUE 8
#define ENV_R_READ      (1ull << 0)
#define ENV_R_WRITE     (1ull << 1)
#define ENV_R_STAT      (1ull << 3)
#define ENV_R_SEEK      (1ull << 4)
#define ENV_R_CONNECT   (1ull << 9)
#define ENV_R_ACCEPT    (1ull << 10)
#define ENV_F_KILL      (1u << 0)

/* Must match kernel struct a20_env_policy (kernel/include/ipc/envelope.h). */
struct env_policy {
    unsigned int allowed_types;
    unsigned long long rights_by_class[32];
    unsigned long long time_budget_ns;
    unsigned long long op_budget;
    unsigned long long data_budget;
    unsigned int propagation_types;
    unsigned int flags;
};

#endif /* A20_CMDS_ENVELOPE_ABI_H */
