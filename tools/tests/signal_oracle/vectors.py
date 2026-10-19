#!/usr/bin/env python3
"""Signal oracle vector catalog (free support task, P2/P3 baseline 8a85da5b).

Expected values are authored from a direct reading of:
  kernel/proc/signal.c            (sha256 32d2714c...)
  kernel/ipc/signalfd.c           (sha256 d96c991d...)
  kernel/abi/linux/sys_signal.c   (sha256 e3cf9b43...)
  kernel/abi/linux/sys_signalfd.c (sha256 2ce65c60...)
  kernel/abi/native/handle_table.c signal section (a20_ht_sig_*, a20_thread_sig_*)
No production file was modified to produce this catalog.

Ops (data only; see model_py.py for the executing model):
  action SIG KIND FLAGS MASK_SIGLIST   KIND in {DFL, IGN, H}
  queue SCOPE SIG INFO|None            SCOPE in {proc, thr}
  deliver / kdeliver / boundary / sigreturn
  timedwait MASK_SIGLIST POLL
  maskop HOW SET_SIGLIST|None
  sigsuspend SET_SIGLIST / restore / wake
  exec / actions SIGLIST
  tmpmask SET / tmpres
  sfdcreate MASK / sfdmask MASK / sfdread COUNT NONBLOCK / sfdpoll
  sfdpark / sfdresume / wakes
  restart
  np SCOPE SIG / ncheck MODE SELECTED / nmask MASK / nblock MASK / ntake / nstate
"""
NSIG = 64
DFL, IGN, H = 0, 1, 2
SA_RESTART = 0x10000000
SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK = 0, 1, 2

HUP, INT, QUIT, KILL, USR1, SEGV, USR2, PIPE, ALRM, TERM = 1, 2, 3, 9, 10, 11, 12, 13, 14, 15
CHLD, CONT, STOP, TSTP, TTIN, TTOU, URG, WINCH, SYS, RT63 = 17, 18, 19, 20, 21, 22, 23, 28, 31, 63

S = dict(HUP=HUP, INT=INT, QUIT=QUIT, KILL=KILL, USR1=USR1, SEGV=SEGV, USR2=USR2,
         PIPE=PIPE, ALRM=ALRM, TERM=TERM, CHLD=CHLD, CONT=CONT, STOP=STOP,
         TSTP=TSTP, TTIN=TTIN, TTOU=TTOU, URG=URG, WINCH=WINCH, SYS=SYS, RT63=RT63)

VECTORS = []


def V(vid, area, ops, expect, cmodel=True, note=""):
    VECTORS.append(dict(id=vid, area=area, ops=ops, expect=expect, cmodel=cmodel, note=note))


# ------------------------- Q: queue ordering / coalescing -------------------------
V('Q01', 'queue', [('action', USR1, H, 0, []), ('queue', 'proc', USR1, 'user'),
                   ('queue', 'proc', USR1, 'user'), ('deliver',)],
  dict(pending='-', ev='handler:10:user:old=-'), note='standard signals coalesce in one scope')
V('Q02', 'queue', [('action', USR1, H, 0, []), ('queue', 'proc', USR1, 'user'),
                   ('queue', 'proc', USR1, 'rt7'), ('timedwait', [USR1], 1)],
  dict(res='10', info='rt7', pending='-'), note='process scope last-info-wins')
V('Q03', 'queue', [('queue', 'thr', USR1, 'user'), ('queue', 'thr', USR1, 'rt7'),
                   ('timedwait', [USR1], 1)],
  dict(res='10', info='user', pending='-', thread='-'), note='thread scope first-info-wins')
V('Q04', 'queue', [('queue', 'proc', USR1, 'user'), ('queue', 'proc', USR1, None),
                   ('timedwait', [USR1], 1)],
  dict(res='10', info='sigonly', pending='-'), note='no-info queue clears stored process has_info')
V('Q05', 'queue', [('queue', 'thr', USR1, None), ('queue', 'thr', USR1, 'user'),
                   ('timedwait', [USR1], 1)],
  dict(res='10', info='sigonly', pending='-', thread='-'),
  note='thread first occurrence had no info; later info is dropped')
V('Q06', 'queue', [('action', CHLD, DFL, 0, []), ('action', URG, DFL, 0, []),
                   ('action', WINCH, DFL, 0, []), ('action', CONT, DFL, 0, []),
                   ('queue', 'proc', CHLD, 'cldexit'), ('queue', 'proc', URG, 'user'),
                   ('queue', 'proc', WINCH, 'user'), ('queue', 'proc', CONT, None)],
  dict(pending='18'), note='DFL default-ignore discarded at queue; SIGCONT always queued')
V('Q07', 'queue', [('action', CHLD, DFL, 0, []), ('queue', 'proc', CHLD, 'cldexit'),
                   ('action', CHLD, H, 0, []), ('queue', 'proc', CHLD, 'user'), ('deliver',)],
  dict(pending='-', ev='handler:17:user:old=-'), note='discard decided at queue time')
V('Q08', 'queue', [('action', USR1, H, 0, []), ('queue', 'proc', USR1, 'user'),
                   ('action', USR1, IGN, 0, []), ('deliver',)],
  dict(pending='-', ev='drop:10'), note='action change to IGN honored at delivery')
V('Q09', 'queue', [('queue', 'proc', STOP, None), ('queue', 'thr', STOP, None),
                   ('queue', 'proc', CONT, None)],
  dict(pending='18', thread='-'), note='SIGCONT cancels STOP/TSTP/TTIN/TTOU in both scopes')
V('Q10', 'queue', [('queue', 'proc', CONT, None), ('queue', 'thr', CONT, None),
                   ('queue', 'proc', TSTP, None)],
  dict(pending='20', thread='-'), note='stop-default cancels SIGCONT in both scopes')
V('Q11', 'queue', [('action', USR1, H, 0, []), ('queue', 'proc', USR1, 'user'),
                   ('queue', 'thr', USR1, 'tkill'), ('deliver',), ('sigreturn',),
                   ('deliver',), ('sigreturn',)],
  dict(ev_seq='handler:10:tkill:old=-;handler:10:user:old=-', pending='-', thread='-'),
  note='same signal in both scopes: thread occurrence consumed first')
V('Q12', 'queue', [('action', USR1, H, 0, []), ('action', USR2, H, 0, []),
                   ('queue', 'proc', USR2, 'user'), ('queue', 'proc', USR1, 'user'),
                   ('deliver',), ('sigreturn',), ('deliver',), ('sigreturn',)],
  dict(ev_seq='handler:10:user:old=-;handler:12:user:old=-', pending='-'),
  note='lowest-numbered deliverable first')
V('Q13', 'queue', [('queue', 'proc', TERM, 'user'), ('kdeliver',)],
  dict(ev='exit:15:nocore', pending='-'), note='kernel-context DFL terminate keeps WCOREDUMP clear')
V('Q13u', 'queue', [('queue', 'proc', TERM, 'user'), ('deliver',)],
  dict(ev='exit:15:nocore', pending='-'), note='user-context TERM DFL terminate')
V('Q14', 'queue', [('action', SEGV, DFL, 0, []), ('queue', 'proc', SEGV, 'user'), ('deliver',)],
  dict(ev='exit:11:core', pending='-'), note='DFL core-class signal; WCOREDUMP bit depends on hook result')
V('Q15', 'queue', [('action', TSTP, DFL, 0, []), ('queue', 'proc', TSTP, None), ('deliver',)],
  dict(ev='stop:20', pending='-'), note='DFL stop clears pending and stops')
V('Q16', 'queue', [('action', CONT, IGN, 0, []), ('queue', 'proc', CONT, None), ('deliver',)],
  dict(pending='-', ev='drop:18'), note='SIGCONT queued even when IGN; dropped at delivery')

# ------------------------------- M: mask transitions -------------------------------
V('M01', 'mask', [('maskop', SIG_BLOCK, [USR1]), ('maskop', SIG_BLOCK, [USR2]),
                  ('maskop', SIG_UNBLOCK, [USR1]), ('maskop', SIG_SETMASK, [ALRM])],
  dict(mseq='10;10,12;12;14'), note='BLOCK/UNBLOCK/SETMASK transitions')
V('M02', 'mask', [('maskop', SIG_SETMASK, [ALRM]), ('maskop', SIG_BLOCK, [KILL, STOP, USR1]),
                  ('maskop', SIG_SETMASK, [KILL, STOP, USR1])],
  dict(mseq='14;10,14;10'), note='SIGKILL/SIGSTOP bits can never enter the mask')
V('M03', 'mask', [('maskop', 7, [USR1])], dict(res='-22', blocked='-'),
  note='invalid how with non-null set -> EINVAL after any oldset copy')
V('M04', 'mask', [('maskop', 7, None)], dict(res='0', blocked='-'),
  note='set==NULL returns 0 before how validation (compat divergence D1)')
V('M05', 'mask', [('maskop', SIG_BLOCK, [USR2]), ('action', USR2, H, 0, []),
                  ('queue', 'proc', USR2, 'user'), ('sigsuspend', [ALRM]),
                  ('boundary',), ('sigreturn',)],
  dict(ev='handler:12:user:old=12', blocked='12', parked='0'),
  note='sigsuspend replaces mask; USR2 unblocked for the wait; delivery restores prior mask')
V('M06', 'mask', [('action', CHLD, H, 0, []), ('maskop', SIG_BLOCK, [CHLD]),
                  ('queue', 'proc', CHLD, 'user'), ('action', CHLD, IGN, 0, []),
                  ('sigsuspend', []), ('wake',), ('boundary',)],
  dict(ev='drop:17', blocked='17', pending='-', parked='1'),
  note='pending ignored signal does not wake sigsuspend; delivery drops it and restores mask')
V('M07', 'mask', [('action', USR1, H, SA_RESTART, [USR2]), ('action', USR2, H, 0, []),
                  ('queue', 'proc', USR1, 'user'), ('queue', 'thr', USR2, 'user'),
                  ('action', USR2, IGN, 0, []), ('maskop', SIG_BLOCK, [ALRM]), ('exec',),
                  ('actions', [USR1, USR2])],
  dict(act='10:DFL:0:0;12:IGN:0:0', pending='-', thread='-', blocked='14'),
  note='exec: dispositions reset except SIG_IGN; pending cleared; blocked preserved')
V('M08', 'mask', [('maskop', SIG_BLOCK, [USR1]), ('tmpmask', [USR2]), ('tmpres',)],
  dict(tmp_old='10', mseq='10;12;10'), note='set_temporary_mask / restore (epoll_pwait seam)')
V('M09', 'mask', [('sigsuspend', [KILL, STOP, USR1]), ('restore',)],
  dict(blocked='-', res='-4'), note='sigsuspend clears KILL/STOP; syscall-return restore path')

# --------------------------------- R: restart ---------------------------------
V('R01', 'restart', [('action', USR1, H, 0, []), ('queue', 'proc', USR1, 'user'), ('restart',)],
  dict(restart='0'), note='handler without SA_RESTART vetoes restart')
V('R02', 'restart', [('action', USR1, H, SA_RESTART, []), ('queue', 'proc', USR1, 'user'), ('restart',)],
  dict(restart='1'), note='SA_RESTART handler restarts')
V('R03', 'restart', [('queue', 'proc', USR1, 'user'), ('restart',)],
  dict(restart='1'), note='DFL terminating signal does not veto restart')
V('R04', 'restart', [('action', CHLD, H, 0, []), ('queue', 'proc', CHLD, 'user'),
                     ('action', CHLD, IGN, 0, []), ('restart',)],
  dict(restart='1'), note='deliverable ignored signal does not veto restart')
V('R05', 'restart', [('restart',)], dict(restart='0'), note='nothing deliverable -> no restart')
V('R06', 'restart', [('action', USR1, H, 0, []), ('maskop', SIG_BLOCK, [USR1]),
                     ('queue', 'proc', USR1, 'user'), ('restart',)],
  dict(restart='0'), note='blocked non-restart handler is not deliverable and does not veto')
V('R07', 'restart', [('action', USR1, H, 0, []), ('action', USR2, H, SA_RESTART, []),
                     ('queue', 'proc', USR1, 'user'), ('queue', 'proc', USR2, 'user'), ('restart',)],
  dict(restart='0'), note='one non-restart deliverable handler vetoes the whole decision')
V('R08', 'restart', [('action', TSTP, DFL, 0, []), ('queue', 'proc', TSTP, None), ('restart',)],
  dict(restart='1'), note='DFL stop does not veto restart')
V('R09', 'restart', [('action', CONT, DFL, 0, []), ('queue', 'proc', CONT, None), ('restart',)],
  dict(restart='1'), note='pending SIGCONT is ignored for the veto but keeps restart true')

# --------------------------------- F: signalfd ---------------------------------
V('F01', 'signalfd', [('maskop', SIG_BLOCK, [USR1, USR2]), ('queue', 'proc', USR2, 'user'),
                      ('queue', 'proc', USR1, 'user'), ('sfdcreate', [USR1, USR2]),
                      ('sfdread', 128, 1), ('sfdread', 128, 1)],
  dict(rec_seq='10:user;12:user', pending='-'), note='lowest signum first; one record per 128-byte read')
V('F02', 'signalfd', [('sfdcreate', [USR1]), ('sfdread', 128, 1)],
  dict(rec_seq='EAGAIN'), note='empty non-blocking read -> EAGAIN')
V('F03', 'signalfd', [('queue', 'proc', USR1, 'user'), ('sfdcreate', [USR1]), ('sfdread', 127, 1)],
  dict(rec_seq='EINVAL', pending='10'), note='count < sizeof(signalfd_siginfo) -> EINVAL')
V('F04a', 'signalfd', [('queue', 'proc', USR1, 'user'), ('queue', 'proc', USR2, 'user'),
                       ('queue', 'proc', ALRM, 'user'), ('sfdcreate', [USR1, USR2, ALRM]),
                       ('sfdread', 255, 1)],
  dict(rec_seq='10:user', pending='12,14'), note='255 bytes fits exactly one record')
V('F04b', 'signalfd', [('queue', 'proc', USR1, 'user'), ('queue', 'proc', USR2, 'user'),
                       ('queue', 'proc', ALRM, 'user'), ('sfdcreate', [USR1, USR2, ALRM]),
                       ('sfdread', 256, 1)],
  dict(rec_seq='10:user;12:user', pending='14'), note='256 bytes fits two records')
V('F04c', 'signalfd', [('queue', 'proc', USR1, 'user'), ('queue', 'proc', USR2, 'user'),
                       ('queue', 'proc', ALRM, 'user'), ('sfdcreate', [USR1, USR2, ALRM]),
                       ('sfdread', 383, 1)],
  dict(rec_seq='10:user;12:user', pending='14'), note='383 bytes fits two records, not three')
V('F04d', 'signalfd', [('queue', 'proc', USR1, 'user'), ('queue', 'proc', USR2, 'user'),
                       ('queue', 'proc', ALRM, 'user'), ('sfdcreate', [USR1, USR2, ALRM]),
                       ('sfdread', 384, 1)],
  dict(rec_seq='10:user;12:user;14:user', pending='-'), note='384 bytes fits three records')
V('F05', 'signalfd', [('queue', 'proc', USR1, 'user'), ('queue', 'thr', USR1, 'tkill'),
                      ('sfdcreate', [USR1]), ('sfdread', 256, 1)],
  dict(rec_seq='10:tkill;10:user', pending='-', thread='-'),
  note='two scopes are two occurrences; thread occurrence first')
V('F06', 'signalfd', [('queue', 'proc', USR1, 'user'), ('queue', 'proc', USR2, 'user'),
                      ('sfdcreate', [USR2]), ('sfdread', 128, 1)],
  dict(rec_seq='12:user', pending='10'), note='mask filters; unmatched pending stays')
V('F07', 'signalfd', [('maskop', SIG_BLOCK, [USR2]), ('sfdcreate', [USR1]),
                      ('queue', 'proc', USR1, 'user'), ('sfdmask', [USR2]),
                      ('sfdread', 128, 1), ('queue', 'proc', USR2, 'user'), ('sfdread', 128, 1)],
  dict(rec_seq='EAGAIN;12:user', pending='10'),
  note='mask update applies to the shared fd object; old-mask pending not consumed')
V('F08', 'signalfd', [('queue', 'proc', USR1, None), ('sfdcreate', [USR1]), ('sfdread', 128, 1)],
  dict(rec_seq='10:absent'), note='no queued info -> only ssi_signo is set')
V('F09', 'signalfd', [('sfdcreate', [USR1]), ('sfdpoll',), ('queue', 'proc', USR1, 'user'),
                      ('sfdpoll',), ('sfdread', 128, 1), ('sfdpoll',)],
  dict(poll_seq='0;1;0', rec_seq='10:user'), note='poll tracks matching pending bits')
V('F10', 'signalfd', [('maskop', SIG_BLOCK, [USR1]), ('queue', 'proc', USR1, 'user'),
                      ('sfdcreate', [USR1]), ('sfdread', 128, 1), ('boundary',)],
  dict(rec_seq='10:user', ev='-', pending='-'),
  note='blocked pending signal consumed by read; no handler runs')
V('F11', 'signalfd', [('queue', 'proc', RT63, 'user'), ('sfdcreate', [RT63]), ('sfdread', 128, 1)],
  dict(rec_seq='63:user'), note='high signal numbers round-trip through the mask conversion')
V('F12', 'signalfd', [('action', USR1, H, 0, []), ('maskop', SIG_BLOCK, [USR1]),
                      ('queue', 'proc', USR1, 'user'), ('sfdcreate', [USR1]),
                      ('sfdread', 128, 1), ('maskop', SIG_SETMASK, []), ('boundary',)],
  dict(rec_seq='10:user', ev='-'), note='handler does not run for a signalfd-consumed signal')
V('F13', 'signalfd', [('maskop', SIG_BLOCK, [USR1, USR2]), ('sfdcreate', [USR1]),
                      ('sfdpark',), ('queue', 'proc', USR2, 'user'), ('wakes',),
                      ('sfdmask', [USR2]), ('wakes',), ('sfdresume',)],
  dict(parked='1', wake_seq='0;1', rec_seq='12:user'), cmodel=False,
  note='blocked non-matching queue does not wake; mask update does')
V('F14', 'signalfd', [('maskop', SIG_BLOCK, [USR2]), ('sfdcreate', [USR2]),
                      ('sfdpark',), ('action', USR1, H, 0, []), ('queue', 'proc', USR1, 'user'),
                      ('wakes',), ('sfdresume',)],
  dict(parked='1', wake_seq='1', rec_seq='EINTR'), cmodel=False,
  note='unblocked unrelated signal interrupts a parked read with EINTR')
V('F15', 'signalfd', [('sfdcreate', [USR1]), ('sfdpark',), ('action', USR1, H, 0, []),
                      ('queue', 'proc', USR1, 'user'), ('wakes',), ('sfdresume',)],
  dict(parked='1', wake_seq='1', rec_seq='10:user'), cmodel=False,
  note='matching signal is consumed, not reported as EINTR')

# ------------------------------ N: native checkpoints ------------------------------
V('N01', 'native', [('np', 'thr', USR1), ('ncheck', 0, []), ('nstate',)],
  dict(nres='10', npending='-'), note='thread-scope checkpoint take')
V('N02', 'native', [('np', 'proc', USR1), ('ncheck', 0, []), ('ncheck', 0, []), ('nstate',)],
  dict(nres_seq='10;-', nht='-'), note='process-scope take clears ht bit')
V('N03', 'native', [('np', 'thr', USR1), ('ncheck', 2, []), ('ncheck', 0, []), ('nstate',)],
  dict(nres_seq='10;10', npending='-'), note='PEEK leaves pending')
V('N04', 'native', [('np', 'thr', USR1), ('np', 'thr', USR2), ('ncheck', 0, [])],
  dict(nres='10,12'), note='no SELECT returns the whole deliverable bitmap')
V('N05', 'native', [('np', 'thr', USR2), ('np', 'proc', USR1), ('ncheck', 4, [USR1, USR2]),
                    ('ncheck', 4, [USR1, USR2]), ('nstate',)],
  dict(nres_seq='10;12', npending='-', nht='-'), note='SELECT consumes one lowest signal per call')
V('N06', 'native', [('np', 'thr', USR1), ('np', 'proc', USR2), ('ncheck', 6, [USR1, USR2]),
                    ('nstate',)],
  dict(nres='10,12', npending='10', nht='12'), note='SELECT+PEEK reports without consuming')
V('N07', 'native', [('np', 'thr', USR1), ('np', 'proc', USR1), ('ncheck', 0, []),
                    ('ncheck', 0, []), ('nstate',)],
  dict(nres_seq='10;10', npending='-', nht='-'), note='thread occurrence preferred; two occurrences remain')
V('N08', 'native', [('nmask', [KILL, STOP, USR1, RT63]), ('nblock', [KILL, STOP, USR1])],
  dict(natmask='10,63', htblock='10,19'), note='thread mask drops KILL/STOP/bit0; ht blocked keeps STOP (D3)')
V('N09', 'native', [('nblock', [USR1]), ('np', 'proc', USR1), ('ncheck', 0, []),
                    ('np', 'proc', USR1), ('ntake',), ('nstate',)],
  dict(nres='10', ntake='-', nht='10'), note='thread check ignores ht blocked; process take respects it (D3)')
V('N10', 'native', [('nmask', [USR1]), ('np', 'proc', USR1), ('ncheck', 4, [USR1]), ('nstate',)],
  dict(nres='10', npending='-'), note='SELECT consumes even a blocked signal (sigwait semantics)')
