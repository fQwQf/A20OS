/* Session ownership, foreground process groups and revocation shared by the
 * console and PTYs. Terminal slots are permanent; association and I/O epochs
 * prevent a reused PTY from inheriting a dead session or old file authority. */
#include "fs/tty_control.h"
#include "fs/tty.h"
#include "fs/vfs.h"
#include "core/lock.h"
#include "core/consts.h"
#include "core/ioctl.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "proc/userns.h"
#include "sys/usercopy.h"

typedef struct tty_control_state {
    uint64_t association, io_epoch;
    int sid, pgrp;
} tty_control_state_t;
static tty_control_state_t terminals[TTY_CONTROL_PTYS + 2];
/* Lock order: pair lock -> control_lock. Never held across task-list
 * traversal, signal delivery, capability queries or driver callbacks. */
static spinlock_t control_lock = SPINLOCK_INIT;

static task_t *process(task_t *t) { return t && t->tg_leader ? t->tg_leader : t; }
static int valid_id(uint32_t id) { return id && id < sizeof(terminals)/sizeof(terminals[0]); }
static tty_control_state_t *state(uint32_t id) {
    if (!valid_id(id)) return NULL;
    tty_control_state_t *s = &terminals[id];
    if (!s->association) s->association = 1;
    if (!s->io_epoch) s->io_epoch = 1;
    return s;
}
static void advance(uint64_t *generation) { if (++*generation == 0) ++*generation; }
static int associated(task_t *p, uint32_t id, tty_control_state_t *s) {
    return p && s && p->ctty_id == id && p->ctty_generation == s->association &&
           p->sid == s->sid && s->sid > 0;
}
static int has_terminal(task_t *p) {
    return p && associated(p, p->ctty_id, state(p->ctty_id));
}
static void assign(task_t *p, uint32_t id, tty_control_state_t *s) {
    advance(&s->association);
    s->sid = p->sid;
    s->pgrp = p->pgid;
    p->ctty_id = id;
    p->ctty_generation = s->association;
}

void tty_control_inherit(task_t *t, task_t *parent) {
    task_t *p = process(parent);
    uint64_t f = spin_lock_irqsave(&control_lock);
    t->ctty_id = has_terminal(p) ? p->ctty_id : 0;
    t->ctty_generation = t->ctty_id ? p->ctty_generation : 0;
    spin_unlock_irqrestore(&control_lock, f);
}
void tty_control_new_session(task_t *task) {
    task_t *p = process(task);
    if (!p) return;
    uint64_t f = spin_lock_irqsave(&control_lock);
    p->ctty_id = 0;
    p->ctty_generation = 0;
    spin_unlock_irqrestore(&control_lock, f);
}
void tty_control_reset(uint32_t id) {
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(id);
    if (s) {
        advance(&s->association);
        advance(&s->io_epoch);
        s->sid = s->pgrp = 0;
    }
    spin_unlock_irqrestore(&control_lock, f);
}
void tty_control_bind(vfile_t *vf, uint32_t id, int master, int flags) {
    task_t *p = process(proc_current());
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(id);
    if (s) {
        vf->tty_id = id;
        vf->tty_master = master != 0;
        vf->tty_io_epoch = s->io_epoch;
        if (!master && !(flags & O_NOCTTY) && (flags & O_ACCMODE) != O_WRONLY && p && p->sid == p->tgid &&
            !has_terminal(p) && !s->sid)
            assign(p, id, s);
    }
    spin_unlock_irqrestore(&control_lock, f);
}
uint32_t tty_control_current(uint64_t *io_epoch) {
    task_t *p = process(proc_current());
    uint64_t f = spin_lock_irqsave(&control_lock);
    uint32_t id = has_terminal(p) ? p->ctty_id : 0;
    if (io_epoch) *io_epoch = id ? state(id)->io_epoch : 0;
    spin_unlock_irqrestore(&control_lock, f);
    return id;
}
int tty_control_revoked(const vfile_t *vf) {
    if (!vf || !vf->tty_id || vf->tty_master) return 0;
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(vf->tty_id);
    int revoked = !s || s->io_epoch != vf->tty_io_epoch;
    spin_unlock_irqrestore(&control_lock, f);
    return revoked;
}
uint64_t tty_control_io_epoch(uint32_t id) {
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(id);
    uint64_t epoch = s ? s->io_epoch : 0;
    spin_unlock_irqrestore(&control_lock, f);
    return epoch;
}
int tty_control_foreground(uint32_t id) {
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(id);
    int pgid = s ? s->pgrp : 0;
    spin_unlock_irqrestore(&control_lock, f);
    return pgid;
}
void tty_control_signal_foreground(uint32_t id, int sig) {
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(id);
    int sid = s ? s->sid : 0, pgrp = s ? s->pgrp : 0;
    spin_unlock_irqrestore(&control_lock, f);
    if (sid && pgrp) proc_tty_signal_group(sid, pgrp, sig);
}
int tty_control_access(vfile_t *vf, int writing, int force_tostop) {
    if (!vf || !vf->tty_id || vf->tty_master) return 0;
    if (tty_control_revoked(vf)) return -EIO;
    if (writing && !force_tostop) {
        ktty_termios_t tio;
        if (tty_vfile_get_termios(vf, &tio) < 0 || !(tio.c_lflag & 0x100 /* TOSTOP */))
            return 0;
    }
    task_t *t = proc_current(), *p = process(t);
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(vf->tty_id);
    int sid = associated(p, vf->tty_id, s) ? s->sid : 0;
    int pgid = sid ? p->pgid : 0;
    int background = sid && s->pgrp && pgid != s->pgrp;
    spin_unlock_irqrestore(&control_lock, f);
    if (!background) return 0;
    int sig = writing ? SIGTTOU : SIGTTIN;
    if (signal_task_blocks_or_ignores(t, sig)) return writing ? 0 : -EIO;
    if (proc_tty_group_orphaned(sid, pgid)) return -EIO;
    proc_tty_signal_group(sid, pgid, sig);
    return -ERESTARTSYS;
}
int tty_control_is_ioctl(unsigned long request) {
    return request == TIOCSCTTY || request == TIOCNOTTY || request == TIOCGSID ||
           request == TIOCGPGRP || request == TIOCSPGRP;
}
int64_t tty_control_ioctl(vfile_t *vf, unsigned long request, void *arg) {
    if (!vf || !valid_id(vf->tty_id)) return -ENOTTY;
    if (tty_control_revoked(vf)) return -EIO;
    task_t *p = process(proc_current());
    if (!p) return -ENOTTY;
    int value = 0;
    if (request == TIOCSPGRP) {
        uint64_t flags = spin_lock_irqsave(&control_lock);
        int owned = associated(p, vf->tty_id, state(vf->tty_id));
        spin_unlock_irqrestore(&control_lock, flags);
        if (!owned) return -ENOTTY;
        int r = tty_control_access(vf, 1, 1);
        if (r < 0) return r;
        if (copy_from_user(&value, arg, sizeof(value)) < 0) return -EFAULT;
        if (value < 0) return -EINVAL;
        int sid = 0;
        if (!proc_tty_group_session(value, &sid)) return -ESRCH;
        if (sid != p->sid) return -EPERM;
    }
    int can_steal = request == TIOCSCTTY && (uintptr_t)arg == 1 &&
        userns_capable(proc_current(), userns_init_ns(), CAP_SYS_ADMIN);
    int can_claim_writeonly = (vf->flags & O_ACCMODE) != O_WRONLY ||
        userns_capable(proc_current(), userns_init_ns(), CAP_SYS_ADMIN);
    tty_hangup_notice_t notify = {0};
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(vf->tty_id);
    int owned = associated(p, vf->tty_id, s);
    int error = 0;
    if (!vf->tty_master && vf->tty_io_epoch != s->io_epoch) error = -EIO;
    else if (request == TIOCSCTTY) {
        if (p->sid != p->tgid) error = -EPERM;
        else if (owned) error = 0;
        else if (has_terminal(p) || (s->sid && !can_steal)) error = -EPERM;
        else if (!can_claim_writeonly) error = -EPERM;
        else assign(p, vf->tty_id, s);
    } else if (request == TIOCNOTTY) {
        if (!owned) error = -ENOTTY;
        else {
            p->ctty_id = 0;
            p->ctty_generation = 0;
            if (p->sid == p->tgid) {
                notify = (tty_hangup_notice_t){s->sid, s->pgrp, 0};
                s->sid = s->pgrp = 0;
                advance(&s->association);
            }
        }
    } else if (!owned && !(vf->tty_master && (request == TIOCGPGRP || request == TIOCGSID))) {
        error = -ENOTTY;
    } else if (request == TIOCSPGRP) {
        s->pgrp = value;
    } else if (request == TIOCGPGRP) value = s->pgrp;
    else if (request == TIOCGSID) value = s->sid;
    else error = -ENOTTY;
    spin_unlock_irqrestore(&control_lock, f);
    tty_control_hangup_notify(notify);
    if (error) return error;
    if (request == TIOCGPGRP || request == TIOCGSID)
        return copy_to_user(arg, &value, sizeof(value)) < 0 ? -EFAULT : 0;
    return 0;
}
tty_hangup_notice_t tty_control_hangup_prepare(uint32_t id, int revoke_io) {
    tty_hangup_notice_t notice = {0};
    uint64_t f = spin_lock_irqsave(&control_lock);
    tty_control_state_t *s = state(id);
    if (s) {
        notice = (tty_hangup_notice_t){s->sid, s->pgrp, revoke_io != 0};
        s->sid = s->pgrp = 0;
        advance(&s->association);
        if (revoke_io) advance(&s->io_epoch);
    }
    spin_unlock_irqrestore(&control_lock, f);
    return notice;
}
void tty_control_hangup_notify(tty_hangup_notice_t notice) {
    if (notice.leader_only && notice.sid) {
        proc_tty_signal_leader(notice.sid, SIGHUP);
        proc_tty_signal_leader(notice.sid, SIGCONT);
    } else if (notice.sid && notice.pgrp) {
        proc_tty_signal_group(notice.sid, notice.pgrp, SIGHUP);
        proc_tty_signal_group(notice.sid, notice.pgrp, SIGCONT);
    }
}
void tty_control_task_exit(task_t *t) {
    task_t *p = process(t);
    if (!p || p->sid != p->tgid || !proc_tty_group_exited(t)) return;
    uint64_t f = spin_lock_irqsave(&control_lock);
    uint32_t id = has_terminal(p) ? p->ctty_id : 0;
    tty_hangup_notice_t notice = {0};
    if (id) {
        tty_control_state_t *s = state(id);
        notice = (tty_hangup_notice_t){s->sid, s->pgrp, 0};
        s->sid = s->pgrp = 0;
        advance(&s->association);
    }
    p->ctty_id = 0;
    p->ctty_generation = 0;
    spin_unlock_irqrestore(&control_lock, f);
    tty_control_hangup_notify(notice);
}
int tty_control_vhangup(void) {
    if (!userns_capable(proc_current(), userns_init_ns(), CAP_SYS_TTY_CONFIG)) return -EPERM;
    task_t *p = process(proc_current());
    uint64_t flags = spin_lock_irqsave(&control_lock);
    uint32_t id = has_terminal(p) ? p->ctty_id : 0;
    tty_hangup_notice_t notice = {0};
    uint64_t epoch = 0;
    if (id) {
        tty_control_state_t *s = state(id);
        notice = (tty_hangup_notice_t){s->sid, s->pgrp, 1};
        s->sid = s->pgrp = 0;
        advance(&s->association);
        advance(&s->io_epoch);
        epoch = s->io_epoch;
    }
    spin_unlock_irqrestore(&control_lock, flags);
    if (!id) return 0;
    /* Wake blocked readers after changing the epoch, before notifying jobs. */
    if (id == TTY_CONSOLE_ID) {
        extern void uart_tty_wake(void);
        uart_tty_wake();
    } else {
        extern void pty_hangup_wake(int, uint64_t);
        pty_hangup_wake((int)id - 2, epoch);
    }
    tty_control_hangup_notify(notice);
    return 0;
}
