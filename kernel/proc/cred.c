#include "proc/cred.h"
#include "core/errno.h"

/*
 * POSIX credential transition rules — ABI-agnostic process subsystem.
 *
 * Moved verbatim from abi/linux/{sys_proc,sys_capability}.c: saved-id
 * semantics for the set*uid/set*gid family, the uid-transition capability
 * recalculation and the capset(2) EPERM matrix.  Privilege is expressed
 * through CAP_SETUID/CAP_SETGID via proc_has_cap().
 */

static int cred_is_root(task_t *t) {
    return proc_has_cap(t, CAP_SETUID);
}

static int cred_can_setgid(task_t *t) {
    return proc_has_cap(t, CAP_SETGID);
}

static int uid_is_current(task_t *t, int uid) {
    return uid == t->cred.uid || uid == t->cred.euid || uid == t->cred.suid;
}

static int gid_is_current(task_t *t, int gid) {
    return gid == t->cred.gid || gid == t->cred.egid || gid == t->cred.sgid;
}

static void cred_update_uid_caps(task_t *t, int old_uid, int old_euid,
                                 int old_suid) {
    if (!t) return;
    int old_had_root = old_uid == 0 || old_euid == 0 || old_suid == 0;
    int new_has_root = t->cred.uid == 0 || t->cred.euid == 0 || t->cred.suid == 0;

    if (old_had_root && !new_has_root) {
        t->cred.cap_effective = 0;
        t->cred.cap_permitted = 0;
        return;
    }
    if (old_euid == 0 && t->cred.euid != 0) {
        t->cred.cap_effective = 0;
        return;
    }
    if (old_euid != 0 && t->cred.euid == 0)
        t->cred.cap_effective = t->cred.cap_permitted;
}

int cred_setuid(task_t *t, int uid) {
    if (cred_is_root(t)) {
        int old_uid = t->cred.uid, old_euid = t->cred.euid, old_suid = t->cred.suid;
        t->cred.uid = t->cred.euid = t->cred.suid = t->cred.fsuid = uid;
        cred_update_uid_caps(t, old_uid, old_euid, old_suid);
        return 0;
    }
    if (!uid_is_current(t, uid)) return -EPERM;
    int old_uid = t->cred.uid, old_euid = t->cred.euid, old_suid = t->cred.suid;
    t->cred.euid = t->cred.fsuid = uid;
    cred_update_uid_caps(t, old_uid, old_euid, old_suid);
    return 0;
}

int cred_setgid(task_t *t, int gid) {
    if (cred_can_setgid(t)) {
        t->cred.gid = t->cred.egid = t->cred.sgid = t->cred.fsgid = gid;
        return 0;
    }
    if (!gid_is_current(t, gid)) return -EPERM;
    t->cred.egid = t->cred.fsgid = gid;
    return 0;
}

int cred_setreuid(task_t *t, int ruid, int euid) {
    if (!cred_is_root(t)) {
        if (ruid != -1 && !uid_is_current(t, ruid)) return -EPERM;
        if (euid != -1 && !uid_is_current(t, euid)) return -EPERM;
    }
    int old_uid = t->cred.uid, old_euid = t->cred.euid, old_suid = t->cred.suid;
    if (ruid != -1) t->cred.uid = ruid;
    if (euid != -1) t->cred.euid = t->cred.fsuid = euid;
    if (ruid != -1 || euid != -1) t->cred.suid = t->cred.euid;
    cred_update_uid_caps(t, old_uid, old_euid, old_suid);
    return 0;
}

int cred_setregid(task_t *t, int rgid, int egid) {
    if (!cred_can_setgid(t)) {
        if (rgid != -1 && !gid_is_current(t, rgid)) return -EPERM;
        if (egid != -1 && !gid_is_current(t, egid)) return -EPERM;
    }
    if (rgid != -1) t->cred.gid = rgid;
    if (egid != -1) t->cred.egid = t->cred.fsgid = egid;
    if (rgid != -1 || egid != -1) t->cred.sgid = t->cred.egid;
    return 0;
}

int cred_setresuid(task_t *t, int ruid, int euid, int suid) {
    if (!cred_is_root(t)) {
        if (ruid != -1 && !uid_is_current(t, ruid)) return -EPERM;
        if (euid != -1 && !uid_is_current(t, euid)) return -EPERM;
        if (suid != -1 && !uid_is_current(t, suid)) return -EPERM;
    }
    int old_uid = t->cred.uid, old_euid = t->cred.euid, old_suid = t->cred.suid;
    if (ruid != -1) t->cred.uid = ruid;
    if (euid != -1) t->cred.euid = t->cred.fsuid = euid;
    if (suid != -1) t->cred.suid = suid;
    cred_update_uid_caps(t, old_uid, old_euid, old_suid);
    return 0;
}

int cred_setresgid(task_t *t, int rgid, int egid, int sgid) {
    if (!cred_can_setgid(t)) {
        if (rgid != -1 && !gid_is_current(t, rgid)) return -EPERM;
        if (egid != -1 && !gid_is_current(t, egid)) return -EPERM;
        if (sgid != -1 && !gid_is_current(t, sgid)) return -EPERM;
    }
    if (rgid != -1) t->cred.gid = rgid;
    if (egid != -1) t->cred.egid = t->cred.fsgid = egid;
    if (sgid != -1) t->cred.sgid = sgid;
    return 0;
}

int cred_setfsuid(task_t *t, int uid) {
    int old = t->cred.fsuid;
    if (uid >= 0 && (cred_is_root(t) || uid_is_current(t, uid)))
        t->cred.fsuid = uid;
    return old;
}

int cred_setfsgid(task_t *t, int gid) {
    int old = t->cred.fsgid;
    if (gid >= 0 && (cred_can_setgid(t) || gid_is_current(t, gid)))
        t->cred.fsgid = gid;
    return old;
}

int cred_capset_apply(proc_cred_t *cred, uint64_t new_effective,
                      uint64_t new_permitted, uint64_t new_inheritable) {
    uint64_t old_effective = cred->cap_effective;
    uint64_t old_permitted = cred->cap_permitted;
    uint64_t old_inheritable = cred->cap_inheritable;

    if ((new_effective & ~new_permitted) != 0)
        return -EPERM;
    if ((new_permitted & ~old_permitted) != 0)
        return -EPERM;
    uint64_t inheritable_allowed = old_inheritable | old_permitted;
    if (old_effective & (1ULL << CAP_SETPCAP))
        inheritable_allowed |= cred->cap_bounding;
    if ((new_inheritable & ~inheritable_allowed) != 0)
        return -EPERM;

    cred->cap_effective = new_effective;
    cred->cap_permitted = new_permitted;
    cred->cap_inheritable = new_inheritable;
    return 0;
}
