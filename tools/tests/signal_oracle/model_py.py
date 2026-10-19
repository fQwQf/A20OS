#!/usr/bin/env python3
"""Abstract signal model transcribed from the frozen P2/P3 C sources.

This model executes the vectors in vectors.py and compares the result with the
authored expectations.  It is an independent oracle for the migration, not
production validation.  Python is used directly (no kernel headers); the
transcription is line-by-line against:
  kernel/proc/signal.c, kernel/ipc/signalfd.c,
  kernel/abi/linux/sys_signal.c, kernel/abi/linux/sys_signfd.c,
  kernel/abi/native/handle_table.c (a20_ht_sig_*, a20_thread_sig_*).
"""
import sys
import vectors
from vectors import (NSIG, DFL, IGN, H, S, SA_RESTART, SIG_BLOCK, SIG_UNBLOCK,
                     SIG_SETMASK)

V = vectors.VECTORS


def bit(sig):
    return 1 << sig


def siglist(mask):
    return ",".join(str(s) for s in range(1, NSIG) if mask & bit(s)) or "-"


def bits(seq):
    m = 0
    for s in seq:
        m |= bit(s)
    return m


def lowest(mask):
    for s in range(1, NSIG):
        if mask & bit(s):
            return s
    return 0


DEFAULT_IGNORE = {S['CHLD'], S['URG'], S['WINCH'], S['CONT']}
DEFAULT_STOP = {S['STOP'], S['TSTP'], S['TTIN'], S['TTOU']}
DEFAULT_TERMINATE = set(range(1, NSIG)) - DEFAULT_IGNORE - DEFAULT_STOP
CORE_DEFAULT = {3, 4, 6, 7, 8, 11, 31}


class Task:
    def __init__(self):
        self.is_user = True
        self.pending = 0
        self.thread_pending = 0
        self.blocked = 0
        self.actions = {s: [DFL, 0, 0] for s in range(NSIG)}
        self.proc_has = [False] * NSIG
        self.proc_info = [None] * NSIG
        self.thr_obj = False
        self.thr_has = [False] * NSIG
        self.thr_info = [None] * NSIG
        self.sigsuspend_active = False
        self.sigsuspend_old = 0
        self.sigwait_active = False
        self.sigwait_mask = 0
        self.sig_handling = 0
        self.sig_old_blocked = 0
        self._tmp_old = 0
        # signalfd
        self.sfd_mask = 0
        self.parked = False
        self.park_mask = 0
        self.pending_wake = False
        # native
        self.ht_pending = 0
        self.ht_blocked = 0
        self.nat_pending = 0
        self.nat_blocked = 0

    # ---- signal_clear_pending_locked ----
    def clear(self, sig):
        b = bit(sig)
        if self.thread_pending & b:
            self.thread_pending &= ~b
            if self.thr_obj:
                self.thr_has[sig] = False
        else:
            self.pending &= ~b
            self.proc_has[sig] = False

    def peek(self, sig):
        if self.thread_pending & bit(sig):
            if self.thr_obj and self.thr_has[sig]:
                return self.thr_info[sig]
            return None
        if self.proc_has[sig]:
            return self.proc_info[sig]
        return None

    def take(self, sig):
        info = self.peek(sig)
        self.clear(sig)
        return info

    def deliverable(self):
        return (self.pending | self.thread_pending) & ~self.blocked

    # ---- signal_apply_generation_rules_locked ----
    def generation(self, sig):
        if sig == S['CONT']:
            for s in (S['STOP'], S['TSTP'], S['TTIN'], S['TTOU']):
                self.clear(s)
                self.clear(s)
        elif sig in DEFAULT_STOP:
            self.clear(S['CONT'])
            self.clear(S['CONT'])

    def discarded_at_queue(self, sig):
        kind = self.actions[sig][0]
        return sig != S['CONT'] and (kind == IGN or (kind == DFL and sig in DEFAULT_IGNORE))

    # ---- signal_queue_task ----
    def queue(self, scope, sig, info):
        self.generation(sig)
        if self.discarded_at_queue(sig):
            return False
        if scope == 'thr':
            if info is not None and not self.thr_obj:
                self.thr_obj = True
            if not (self.thread_pending & bit(sig)) and self.thr_obj:
                self.thr_has[sig] = info is not None
                if info is not None:
                    self.thr_info[sig] = info
            self.thread_pending |= bit(sig)
        else:
            if info is not None:
                self.proc_has[sig] = True
                self.proc_info[sig] = info
            else:
                self.proc_has[sig] = False
            self.pending |= bit(sig)
        return True

    def queued_wake(self, sig):
        return bool(self.deliverable()) or bool(self.sigwait_active and (self.sigwait_mask & bit(sig)))

    # ---- delivery ----
    def step_deliver(self, kernel=False):
        d = self.deliverable()
        if not d:
            return None
        sig = lowest(d)
        kind, flags, smask = self.actions[sig]
        if kind == IGN or (kind == DFL and sig in DEFAULT_IGNORE):
            self.clear(sig)
            if self.sigsuspend_active:
                self.blocked = self.sigsuspend_old
                self.sigsuspend_active = False
            return ('drop', sig)
        if kind == DFL and sig in DEFAULT_STOP:
            self.clear(sig)
            if self.sigsuspend_active:
                self.blocked = self.sigsuspend_old
                self.sigsuspend_active = False
            return ('stop', sig)
        if kind == DFL:
            self.clear(sig)
            core = sig in CORE_DEFAULT
            return ('exit', sig, 'nocore' if (core is False or kernel) else 'core')
        info = self.peek(sig)
        self.clear(sig)
        if flags & 0x80000000:  # SA_RESETHAND
            self.actions[sig][0] = DFL
        old = self.sigsuspend_old if self.sigsuspend_active else self.blocked
        self.sig_old_blocked = old
        self.sigsuspend_active = False
        self.blocked |= smask
        if not (flags & 0x40000000):  # SA_NODEFER
            self.blocked |= bit(sig)
        self.sig_handling = sig
        return ('handler', sig, info, old)

    def boundary(self):
        evs = []
        while True:
            e = self.step_deliver()
            if e is None:
                break
            evs.append(e)
            if e[0] in ('handler', 'exit', 'stop'):
                break
        return evs

    def kdeliver(self):
        evs = []
        while True:
            e = self.step_deliver(kernel=True)
            if e is None:
                break
            evs.append(e)
            if e[0] == 'exit':
                break
        return evs

    def sigreturn(self):
        if self.sig_handling:
            self.blocked = self.sig_old_blocked
            self.sig_handling = 0

    # ---- signal_task_should_restart ----
    def should_restart(self):
        d = self.deliverable()
        if not d:
            return False
        for s in range(1, NSIG):
            if not (d & bit(s)):
                continue
            kind, flags, _ = self.actions[s]
            if kind == IGN or (kind == DFL and s in DEFAULT_IGNORE):
                continue
            if kind != DFL and not (flags & SA_RESTART):
                return False
        return True

    # ---- sys_sigtimedwait ----
    def timedwait(self, mask, poll):
        m = (self.pending | self.thread_pending) & mask
        if m:
            sig = lowest(m)
            info = self.take(sig)
            return sig, (info if info is not None else 'sigonly')
        if poll:
            return -11, None
        return None, None

    # ---- signalfd ----
    def sfd_read(self, count, nonblock):
        if count < 128:
            return -22, []
        recs = []
        while True:
            m = (self.pending | self.thread_pending) & self.sfd_mask
            if not m:
                if recs:
                    return len(recs) * 128, recs
                if nonblock:
                    return -11, recs
                return None, recs
            sig = lowest(m)
            info = self.take(sig)
            recs.append((sig, info if info is not None else 'absent'))
            if (len(recs) + 1) * 128 > count:
                return len(recs) * 128, recs

    # ---- masks ----
    def mask_op(self, how, sset):
        if sset is None:
            return 0
        m = sset & ~(bit(S['KILL']) | bit(S['STOP']))
        if how == SIG_BLOCK:
            self.blocked |= m
            return 0
        if how == SIG_UNBLOCK:
            self.blocked &= ~m
            return 0
        if how == SIG_SETMASK:
            self.blocked = m
            return 0
        return -22

    def sigsuspend(self, sset):
        self.sigsuspend_old = self.blocked
        self.sigsuspend_active = True
        self.blocked = sset & ~(bit(S['KILL']) | bit(S['STOP']))
        return self.has_unblocked()

    def has_unblocked(self):
        d = self.deliverable()
        for s in range(1, NSIG):
            if not (d & bit(s)):
                continue
            kind = self.actions[s][0]
            if kind == IGN or (kind == DFL and s in DEFAULT_IGNORE):
                continue
            return True
        return False

    def restore_sigsuspend(self):
        if self.sigsuspend_active and not self.sig_handling:
            self.blocked = self.sigsuspend_old
            self.sigsuspend_active = False

    def temp_mask(self, sset):
        old = self.blocked
        self._tmp_old = old
        self.blocked = sset & ~(bit(S['KILL']) | bit(S['STOP']))
        return old

    def exec_reset(self):
        for s in range(1, NSIG):
            if self.actions[s][0] not in (IGN, DFL):
                self.actions[s][0] = DFL
            self.actions[s][1] = 0
            self.actions[s][2] = 0
        self.pending = 0
        self.thread_pending = 0
        self.proc_has = [False] * NSIG
        self.proc_info = [None] * NSIG
        self.thr_has = [False] * NSIG
        self.thr_info = [None] * NSIG
        self.sig_handling = 0
        self.sigsuspend_active = False
        self.sigwait_active = False
        self.sigwait_mask = 0

    # ---- native checkpoint ops ----
    def native_pend(self, scope, sig):
        if scope == 'thr':
            self.nat_pending |= bit(sig)
        else:
            self.ht_pending |= bit(sig)

    def native_check(self, mode, selected):
        SELECT, PEEK = 4, 2
        pending = self.ht_pending | self.nat_pending
        if mode & SELECT:
            d = pending & selected
            if not (mode & PEEK):
                d &= (0 - d) & ((1 << 64) - 1)
        else:
            d = pending & ~self.nat_blocked
        if not (mode & PEEK):
            local = d & self.nat_pending
            self.nat_pending &= ~local
            self.ht_pending &= ~(d & ~local)
        return d

    def native_thread_mask(self, mask, query):
        old = self.nat_blocked
        if not query:
            self.nat_blocked = mask & ~(bit(9) | bit(19) | 1)
        return old

    def native_ht_block(self, mask):
        old = self.ht_blocked
        self.ht_blocked = mask & ~bit(9)
        return old

    def native_ht_take(self):
        d = self.ht_pending & ~self.ht_blocked
        self.ht_pending &= self.ht_blocked
        return d


KIND_NAME = {DFL: 'DFL', IGN: 'IGN', H: 'H'}


def fmt_ev(e):
    sig = e[1]
    if e[0] == 'handler':
        return "handler:%d:%s:old=%s" % (sig, e[2] or 'absent', siglist(e[3]))
    if e[0] == 'exit':
        return "exit:%d:%s" % (sig, e[2])
    return "%s:%d" % (e[0], sig)


def fmt_rec(rec):
    return "%d:%s" % (rec[0], rec[1])


def run_vector(vec):
    t = Task()
    obs = {'ev_seq': [], 'rec_seq': [], 'poll_seq': [], 'mseq': [], 'nres_seq': [],
           'wake_seq': []}
    for op in vec['ops']:
        name = op[0]
        if name == 'action':
            _, sig, kind, flags, mask = op
            t.actions[sig] = [kind, flags, bits(mask)]
        elif name == 'queue':
            _, scope, sig, info = op
            t.queue(scope, sig, info)
            if t.parked:
                if bool(t.deliverable()) or bool(t.park_mask & bit(sig)):
                    t.pending_wake = True
        elif name == 'deliver':
            evs = t.boundary()
            obs['ev_seq'].append(";".join(fmt_ev(e) for e in evs) or "-")
            obs['ev'] = obs['ev_seq'][-1]
        elif name == 'kdeliver':
            evs = t.kdeliver()
            obs['ev_seq'].append(";".join(fmt_ev(e) for e in evs) or "-")
            obs['ev'] = obs['ev_seq'][-1]
        elif name == 'boundary':
            evs = t.boundary()
            obs['ev_seq'].append(";".join(fmt_ev(e) for e in evs) or "-")
            obs['ev'] = ";" .join(fmt_ev(e) for e in evs) or "-"
        elif name == 'sigreturn':
            t.sigreturn()
        elif name == 'timedwait':
            _, mask, poll = op
            sig, info = t.timedwait(bits(mask), poll)
            if sig is not None:
                obs['res'] = str(sig)
            obs['info'] = str(info)
        elif name == 'maskop':
            _, how, sset = op
            r = t.mask_op(how, bits(sset) if sset is not None else None)
            if r != 0:
                obs['res'] = str(r)
            elif sset is None:
                obs['res'] = '0'
            if sset is not None:
                obs['mseq'].append(siglist(t.blocked))
        elif name == 'sigsuspend':
            _, sset = op
            has = t.sigsuspend(bits(sset))
            obs['res'] = '-4'
            obs['parked'] = '0' if has else '1'
        elif name == 'wake':
            pass
        elif name == 'restore':
            t.restore_sigsuspend()
        elif name == 'exec':
            t.exec_reset()
        elif name == 'actions':
            parts = []
            for sig in op[1]:
                kind, flags, mask = t.actions[sig]
                parts.append("%d:%s:%d:%s" % (sig, KIND_NAME[kind], flags, siglist(mask) if mask else '0'))
            obs['act'] = ";".join(parts)
        elif name == 'tmpmask':
            old = t.temp_mask(bits(op[1]))
            obs['tmp_old'] = siglist(old)
            obs['mseq'].append(siglist(t.blocked))
        elif name == 'tmpres':
            t.blocked = t._tmp_old
            obs['mseq'].append(siglist(t.blocked))
        elif name == 'sfdcreate':
            t.sfd_mask = bits(op[1])
        elif name == 'sfdmask':
            t.sfd_mask = bits(op[1])
            if t.parked:
                t.pending_wake = True
        elif name == 'sfdread':
            _, count, nonblock = op
            res, recs = t.sfd_read(count, nonblock)
            if res == -11:
                obs['rec_seq'].append('EAGAIN')
            elif res == -22:
                obs['rec_seq'].append('EINVAL')
            elif res is None:
                obs['rec_seq'].append('park')
            else:
                obs['rec_seq'].append(";".join(fmt_rec(r) for r in recs))
        elif name == 'sfdpoll':
            obs['poll_seq'].append('1' if ((t.pending | t.thread_pending) & t.sfd_mask) else '0')
        elif name == 'sfdpark':
            t.parked = True
            t.park_mask = t.sfd_mask
            t.pending_wake = False
            obs['parked'] = '1'
        elif name == 'wakes':
            obs['wake_seq'].append('1' if t.pending_wake else '0')
            t.pending_wake = False
        elif name == 'sfdresume':
            m = (t.pending | t.thread_pending) & t.sfd_mask
            if m:
                res, recs = t.sfd_read(128, 0)
                obs['rec_seq'].append(";".join(fmt_rec(r) for r in recs))
            elif t.has_unblocked():
                obs['rec_seq'].append('EINTR')
            else:
                obs['rec_seq'].append('park')
            t.parked = False
        elif name == 'restart':
            obs['restart'] = '1' if t.should_restart() else '0'
        elif name == 'np':
            t.native_pend(op[1], op[2])
        elif name == 'ncheck':
            d = t.native_check(op[1], bits(op[2]))
            obs['nres_seq'].append(siglist(d))
            obs['nres'] = siglist(d)
        elif name == 'nmask':
            t.native_thread_mask(bits(op[1]), 0)
            obs['natmask'] = siglist(t.nat_blocked)
        elif name == 'nblock':
            t.native_ht_block(bits(op[1]))
            obs['htblock'] = siglist(t.ht_blocked)
        elif name == 'ntake':
            obs['ntake'] = siglist(t.native_ht_take())
        elif name == 'nstate':
            obs['npending'] = siglist(t.nat_pending)
            obs['nht'] = siglist(t.ht_pending)
        else:
            raise SystemExit("unknown op " + repr(op))
    # final state observations
    obs['pending'] = siglist(t.pending)
    obs['thread'] = siglist(t.thread_pending)
    obs['blocked'] = siglist(t.blocked)
    # collapse sequences
    for key in ('ev_seq', 'rec_seq', 'poll_seq', 'mseq', 'nres_seq', 'wake_seq'):
        if obs[key]:
            obs[key] = ";".join(obs[key])
        else:
            del obs[key]
    return obs


def main():
    out = open(sys.argv[1] if len(sys.argv) > 1 else 'obs_py.tsv', 'w')
    failures = 0
    npass = 0
    for vec in V:
        obs = run_vector(vec)
        bad = []
        for key, want in vec['expect'].items():
            got = obs.get(key)
            if got != want:
                bad.append((key, want, got))
        if bad:
            failures += 1
            print("FAIL %s (%s)" % (vec['id'], vec['note']))
            print("  ops: %s" % (vec['ops'],))
            for key, want, got in bad:
                print("  %s: expected %r got %r" % (key, want, got))
        else:
            npass += 1
        for key in sorted(obs):
            out.write("%s\t%s\t%s\n" % (vec['id'], key, obs[key]))
    out.close()
    print("vectors: %d pass, %d fail" % (npass, failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
