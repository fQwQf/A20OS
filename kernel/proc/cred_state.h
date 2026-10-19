#ifndef A20_PROC_CRED_STATE_H
#define A20_PROC_CRED_STATE_H

#include "core/types.h"

/*
 * C/Rust ABI for the credential transition policy in kernel/proc/cred_state.rs.
 *
 * `cred_view_t` borrows the live credential fields of one task.  The adapter
 * fills every pointer from `task_t::cred` and keeps whatever lock already
 * guards that credential; Rust performs the reads and writes in place, so the
 * task layout does not change and no credential state is duplicated on the
 * Rust side.
 *
 * `out_flags` receives CRED_FLAG_CLEAR_DUMPABLE when the transition requires
 * the caller to clear mm->dumpable; the adapter performs that MM call.
 */

#define CRED_FLAG_CLEAR_DUMPABLE 0x1u

typedef struct cred_view {
    int *uid;
    int *euid;
    int *suid;
    int *fsuid;
    int *gid;
    int *egid;
    int *sgid;
    int *fsgid;
    uint64_t *cap_effective;
    uint64_t *cap_permitted;
    uint64_t *cap_inheritable;
    uint64_t *cap_bounding;
} cred_view_t;

/* The Rust view is a fixed sequence of twelve pointers. Keep the C mirror
 * byte-compatible: a field reorder or an added pointer must fail to build. */
_Static_assert(sizeof(cred_view_t) == 12 * sizeof(void *),
               "cred_view_t must match the Rust CredView layout");
_Static_assert(__builtin_offsetof(cred_view_t, uid) == 0,
               "cred_view_t.uid offset");
_Static_assert(__builtin_offsetof(cred_view_t, fsuid) == 3 * sizeof(void *),
               "cred_view_t.fsuid offset");
_Static_assert(__builtin_offsetof(cred_view_t, gid) == 4 * sizeof(void *),
               "cred_view_t.gid offset");
_Static_assert(__builtin_offsetof(cred_view_t, fsgid) == 7 * sizeof(void *),
               "cred_view_t.fsgid offset");
_Static_assert(__builtin_offsetof(cred_view_t, cap_effective) ==
                   8 * sizeof(void *),
               "cred_view_t.cap_effective offset");
_Static_assert(__builtin_offsetof(cred_view_t, cap_bounding) ==
                   11 * sizeof(void *),
               "cred_view_t.cap_bounding offset");

int a20_cred_setuid(cred_view_t *v, int uid, int has_cap_setuid,
                    uint32_t *out_flags);
int a20_cred_setgid(cred_view_t *v, int gid, int has_cap_setgid,
                    uint32_t *out_flags);
int a20_cred_setreuid(cred_view_t *v, int ruid, int euid, int has_cap_setuid,
                      uint32_t *out_flags);
int a20_cred_setregid(cred_view_t *v, int rgid, int egid, int has_cap_setgid,
                      uint32_t *out_flags);
int a20_cred_setresuid(cred_view_t *v, int ruid, int euid, int suid,
                       int has_cap_setuid, uint32_t *out_flags);
int a20_cred_setresgid(cred_view_t *v, int rgid, int egid, int sgid,
                       int has_cap_setgid, uint32_t *out_flags);
int a20_cred_setfsuid(cred_view_t *v, int uid, int has_cap_setuid,
                      uint32_t *out_flags);
int a20_cred_setfsgid(cred_view_t *v, int gid, int has_cap_setgid,
                      uint32_t *out_flags);
int a20_cred_capset_apply(cred_view_t *v, uint64_t new_effective,
                          uint64_t new_permitted, uint64_t new_inheritable);

#endif /* A20_PROC_CRED_STATE_H */
