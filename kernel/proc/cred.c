#include "proc/cred.h"
#include "proc/cred_state.h"
#include "core/errno.h"
#include "mm/vm.h"

/*
 * POSIX credential transition rules — ABI-agnostic process subsystem.
 *
 * The transition policy lives in kernel/proc/cred_state.rs: the privilege
 * tests, the saved-id writes, the uid-transition capability recalculation and
 * the capset(2) EPERM matrix.  This file only borrows the live credential
 * fields into a cred_view_t, calls Rust, and performs the one MM side effect
 * Rust asks for (clearing mm->dumpable).
 *
 * Moved verbatim from abi/linux/{sys_proc,sys_capability}.c: saved-id
 * semantics for the set*uid/set*gid family, the uid-transition capability
 * recalculation and the capset(2) EPERM matrix.  Privilege is expressed
 * through CAP_SETUID/CAP_SETGID via proc_has_cap().
 */

static void cred_view_init(cred_view_t *v, proc_cred_t *cred) {
    v->uid = &cred->uid;
    v->euid = &cred->euid;
    v->suid = &cred->suid;
    v->fsuid = &cred->fsuid;
    v->gid = &cred->gid;
    v->egid = &cred->egid;
    v->sgid = &cred->sgid;
    v->fsgid = &cred->fsgid;
    v->cap_effective = &cred->cap_effective;
    v->cap_permitted = &cred->cap_permitted;
    v->cap_inheritable = &cred->cap_inheritable;
    v->cap_bounding = &cred->cap_bounding;
}

static void cred_apply_dumpable(task_t *t, uint32_t flags) {
    if (flags & CRED_FLAG_CLEAR_DUMPABLE)
        mm_set_dumpable(t->mm, 0);
}

int cred_setuid(task_t *t, int uid) {
    if (!t)
        return -EINVAL;
    cred_view_t v;
    uint32_t flags = 0;
    cred_view_init(&v, &t->cred);
    int rc = a20_cred_setuid(&v, uid, proc_has_cap(t, CAP_SETUID), &flags);
    if (rc == 0)
        cred_apply_dumpable(t, flags);
    return rc;
}

int cred_setgid(task_t *t, int gid) {
    if (!t)
        return -EINVAL;
    cred_view_t v;
    uint32_t flags = 0;
    cred_view_init(&v, &t->cred);
    int rc = a20_cred_setgid(&v, gid, proc_has_cap(t, CAP_SETGID), &flags);
    if (rc == 0)
        cred_apply_dumpable(t, flags);
    return rc;
}

int cred_setreuid(task_t *t, int ruid, int euid) {
    if (!t)
        return -EINVAL;
    cred_view_t v;
    uint32_t flags = 0;
    cred_view_init(&v, &t->cred);
    int rc =
        a20_cred_setreuid(&v, ruid, euid, proc_has_cap(t, CAP_SETUID), &flags);
    if (rc == 0)
        cred_apply_dumpable(t, flags);
    return rc;
}

int cred_setregid(task_t *t, int rgid, int egid) {
    if (!t)
        return -EINVAL;
    cred_view_t v;
    uint32_t flags = 0;
    cred_view_init(&v, &t->cred);
    int rc =
        a20_cred_setregid(&v, rgid, egid, proc_has_cap(t, CAP_SETGID), &flags);
    if (rc == 0)
        cred_apply_dumpable(t, flags);
    return rc;
}

int cred_setresuid(task_t *t, int ruid, int euid, int suid) {
    if (!t)
        return -EINVAL;
    cred_view_t v;
    uint32_t flags = 0;
    cred_view_init(&v, &t->cred);
    int rc = a20_cred_setresuid(&v, ruid, euid, suid,
                                proc_has_cap(t, CAP_SETUID), &flags);
    if (rc == 0)
        cred_apply_dumpable(t, flags);
    return rc;
}

int cred_setresgid(task_t *t, int rgid, int egid, int sgid) {
    if (!t)
        return -EINVAL;
    cred_view_t v;
    uint32_t flags = 0;
    cred_view_init(&v, &t->cred);
    int rc = a20_cred_setresgid(&v, rgid, egid, sgid,
                                proc_has_cap(t, CAP_SETGID), &flags);
    if (rc == 0)
        cred_apply_dumpable(t, flags);
    return rc;
}

int cred_setfsuid(task_t *t, int uid) {
    if (!t)
        return -EINVAL;
    cred_view_t v;
    uint32_t flags = 0;
    cred_view_init(&v, &t->cred);
    int old = a20_cred_setfsuid(&v, uid, proc_has_cap(t, CAP_SETUID), &flags);
    cred_apply_dumpable(t, flags);
    return old;
}

int cred_setfsgid(task_t *t, int gid) {
    if (!t)
        return -EINVAL;
    cred_view_t v;
    uint32_t flags = 0;
    cred_view_init(&v, &t->cred);
    int old = a20_cred_setfsgid(&v, gid, proc_has_cap(t, CAP_SETGID), &flags);
    cred_apply_dumpable(t, flags);
    return old;
}

int cred_capset_apply(proc_cred_t *cred, uint64_t new_effective,
                      uint64_t new_permitted, uint64_t new_inheritable) {
    if (!cred)
        return -EINVAL;
    cred_view_t v;
    cred_view_init(&v, cred);
    return a20_cred_capset_apply(&v, new_effective, new_permitted,
                                 new_inheritable);
}
