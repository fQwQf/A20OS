### Group Q — Queue ordering / coalescing / generation (SIGNAL_QUEUE_IMPL, with slice-1 deliverable/fatal seams)

| ID | Model | Ops | Expected observations |
|---|---|---|---|
| Q01 | py+C | act(USR1=H) → q(P,USR1,info=user) → q(P,USR1,info=user) → deliver | pending=-; ev=handler:10:user:old=- |
| Q02 | py+C | act(USR1=H) → q(P,USR1,info=user) → q(P,USR1,info=rt7) → timedwait(USR1,poll=1) | res=10; info=rt7; pending=- |
| Q03 | py+C | q(T,USR1,info=user) → q(T,USR1,info=rt7) → timedwait(USR1,poll=1) | res=10; info=user; pending=-; thread=- |
| Q04 | py+C | q(P,USR1,info=user) → q(P,USR1,noinfo) → timedwait(USR1,poll=1) | res=10; info=sigonly; pending=- |
| Q05 | py+C | q(T,USR1,noinfo) → q(T,USR1,info=user) → timedwait(USR1,poll=1) | res=10; info=sigonly; pending=-; thread=- |
| Q06 | py+C | act(CHLD=DFL) → act(URG=DFL) → act(WINCH=DFL) → act(CONT=DFL) → q(P,CHLD,info=cldexit) → q(P,URG,info=user) → q(P,WINCH,info=user) → q(P,CONT,noinfo) | pending=18 |
| Q07 | py+C | act(CHLD=DFL) → q(P,CHLD,info=cldexit) → act(CHLD=H) → q(P,CHLD,info=user) → deliver | pending=-; ev=handler:17:user:old=- |
| Q08 | py+C | act(USR1=H) → q(P,USR1,info=user) → act(USR1=IGN) → deliver | pending=-; ev=drop:10 |
| Q09 | py+C | q(P,STOP,noinfo) → q(T,STOP,noinfo) → q(P,CONT,noinfo) | pending=18; thread=- |
| Q10 | py+C | q(P,CONT,noinfo) → q(T,CONT,noinfo) → q(P,TSTP,noinfo) | pending=20; thread=- |
| Q11 | py+C | act(USR1=H) → q(P,USR1,info=user) → q(T,USR1,info=tkill) → deliver → sigreturn → deliver → sigreturn | ev_seq=handler:10:tkill:old=-;handler:10:user:old=-; pending=-; thread=- |
| Q12 | py+C | act(USR1=H) → act(USR2=H) → q(P,USR2,info=user) → q(P,USR1,info=user) → deliver → sigreturn → deliver → sigreturn | ev_seq=handler:10:user:old=-;handler:12:user:old=-; pending=- |
| Q13 | py+C | q(P,TERM,info=user) → kdeliver | ev=exit:15:nocore; pending=- |
| Q13u | py+C | q(P,TERM,info=user) → deliver | ev=exit:15:nocore; pending=- |
| Q14 | py+C | act(SEGV=DFL) → q(P,SEGV,info=user) → deliver | ev=exit:11:core; pending=- |
| Q15 | py+C | act(TSTP=DFL) → q(P,TSTP,noinfo) → deliver | ev=stop:20; pending=- |
| Q16 | py+C | act(CONT=IGN) → q(P,CONT,noinfo) → deliver | pending=-; ev=drop:18 |

### Group M — Mask transitions (SIGNAL_CORE_IMPL: a20_signal_mask_apply)

| ID | Model | Ops | Expected observations |
|---|---|---|---|
| M01 | py+C | mask(BLOCK,USR1) → mask(BLOCK,USR2) → mask(UNBLOCK,USR1) → mask(SETMASK,ALRM) | mseq=10;10,12;12;14 |
| M02 | py+C | mask(SETMASK,ALRM) → mask(BLOCK,KILL,STOP,USR1) → mask(SETMASK,KILL,STOP,USR1) | mseq=14;10,14;10 |
| M03 | py+C | mask(7,USR1) | res=-22; blocked=- |
| M04 | py+C | mask(7,NULL) | res=0; blocked=- |
| M05 | py+C | mask(BLOCK,USR2) → act(USR2=H) → q(P,USR2,info=user) → sigsuspend(ALRM) → boundary → sigreturn | ev=handler:12:user:old=12; blocked=12; parked=0 |
| M06 | py+C | act(CHLD=H) → mask(BLOCK,CHLD) → q(P,CHLD,info=user) → act(CHLD=IGN) → sigsuspend([]) → wake → boundary | ev=drop:17; blocked=17; pending=-; parked=1 |
| M07 | py+C | act(USR1=H+RESTART) → act(USR2=H) → q(P,USR1,info=user) → q(T,USR2,info=user) → act(USR2=IGN) → mask(BLOCK,ALRM) → exec → dump-actions(USR1,USR2) | act=10:DFL:0:0;12:IGN:0:0; pending=-; thread=-; blocked=14 |
| M08 | py+C | mask(BLOCK,USR1) → tmpmask(USR2) → tmpres | tmp_old=10; mseq=10;12;10 |
| M09 | py+C | sigsuspend(KILL,STOP,USR1) → restore | blocked=-; res=-4 |

### Group R — Restart decision (SIGNAL_CORE_IMPL, not yet in slice-1 ABI)

| ID | Model | Ops | Expected observations |
|---|---|---|---|
| R01 | py+C | act(USR1=H) → q(P,USR1,info=user) → restart-check | restart=0 |
| R02 | py+C | act(USR1=H+RESTART) → q(P,USR1,info=user) → restart-check | restart=1 |
| R03 | py+C | q(P,USR1,info=user) → restart-check | restart=1 |
| R04 | py+C | act(CHLD=H) → q(P,CHLD,info=user) → act(CHLD=IGN) → restart-check | restart=1 |
| R05 | py+C | restart-check | restart=0 |
| R06 | py+C | act(USR1=H) → mask(BLOCK,USR1) → q(P,USR1,info=user) → restart-check | restart=0 |
| R07 | py+C | act(USR1=H) → act(USR2=H+RESTART) → q(P,USR1,info=user) → q(P,USR2,info=user) → restart-check | restart=0 |
| R08 | py+C | act(TSTP=DFL) → q(P,TSTP,noinfo) → restart-check | restart=1 |
| R09 | py+C | act(CONT=DFL) → q(P,CONT,noinfo) → restart-check | restart=1 |

### Group F — signalfd reads (SIGNAL_SIGNALFD_IMPL)

| ID | Model | Ops | Expected observations |
|---|---|---|---|
| F01 | py+C | mask(BLOCK,USR1,USR2) → q(P,USR2,info=user) → q(P,USR1,info=user) → sfdcreate(USR1,USR2) → sfdread(count=128,nonblock=1) → sfdread(count=128,nonblock=1) | rec_seq=10:user;12:user; pending=- |
| F02 | py+C | sfdcreate(USR1) → sfdread(count=128,nonblock=1) | rec_seq=EAGAIN |
| F03 | py+C | q(P,USR1,info=user) → sfdcreate(USR1) → sfdread(count=127,nonblock=1) | rec_seq=EINVAL; pending=10 |
| F04a | py+C | q(P,USR1,info=user) → q(P,USR2,info=user) → q(P,ALRM,info=user) → sfdcreate(USR1,USR2,ALRM) → sfdread(count=255,nonblock=1) | rec_seq=10:user; pending=12,14 |
| F04b | py+C | q(P,USR1,info=user) → q(P,USR2,info=user) → q(P,ALRM,info=user) → sfdcreate(USR1,USR2,ALRM) → sfdread(count=256,nonblock=1) | rec_seq=10:user;12:user; pending=14 |
| F04c | py+C | q(P,USR1,info=user) → q(P,USR2,info=user) → q(P,ALRM,info=user) → sfdcreate(USR1,USR2,ALRM) → sfdread(count=383,nonblock=1) | rec_seq=10:user;12:user; pending=14 |
| F04d | py+C | q(P,USR1,info=user) → q(P,USR2,info=user) → q(P,ALRM,info=user) → sfdcreate(USR1,USR2,ALRM) → sfdread(count=384,nonblock=1) | rec_seq=10:user;12:user;14:user; pending=- |
| F05 | py+C | q(P,USR1,info=user) → q(T,USR1,info=tkill) → sfdcreate(USR1) → sfdread(count=256,nonblock=1) | rec_seq=10:tkill;10:user; pending=-; thread=- |
| F06 | py+C | q(P,USR1,info=user) → q(P,USR2,info=user) → sfdcreate(USR2) → sfdread(count=128,nonblock=1) | rec_seq=12:user; pending=10 |
| F07 | py+C | mask(BLOCK,USR2) → sfdcreate(USR1) → q(P,USR1,info=user) → sfdmask(USR2) → sfdread(count=128,nonblock=1) → q(P,USR2,info=user) → sfdread(count=128,nonblock=1) | rec_seq=EAGAIN;12:user; pending=10 |
| F08 | py+C | q(P,USR1,noinfo) → sfdcreate(USR1) → sfdread(count=128,nonblock=1) | rec_seq=10:absent |
| F09 | py+C | sfdcreate(USR1) → sfdpoll → q(P,USR1,info=user) → sfdpoll → sfdread(count=128,nonblock=1) → sfdpoll | poll_seq=0;1;0; rec_seq=10:user |
| F10 | py+C | mask(BLOCK,USR1) → q(P,USR1,info=user) → sfdcreate(USR1) → sfdread(count=128,nonblock=1) → boundary | rec_seq=10:user; ev=-; pending=- |
| F11 | py+C | q(P,RT63,info=user) → sfdcreate(RT63) → sfdread(count=128,nonblock=1) | rec_seq=63:user |
| F12 | py+C | act(USR1=H) → mask(BLOCK,USR1) → q(P,USR1,info=user) → sfdcreate(USR1) → sfdread(count=128,nonblock=1) → mask(SETMASK,[]) → boundary | rec_seq=10:user; ev=- |
| F13 | py only | mask(BLOCK,USR1,USR2) → sfdcreate(USR1) → sfdpark → q(P,USR2,info=user) → wake-check → sfdmask(USR2) → wake-check → sfdresume | parked=1; wake_seq=0;1; rec_seq=12:user |
| F14 | py only | mask(BLOCK,USR2) → sfdcreate(USR2) → sfdpark → act(USR1=H) → q(P,USR1,info=user) → wake-check → sfdresume | parked=1; wake_seq=1; rec_seq=EINTR |
| F15 | py only | sfdcreate(USR1) → sfdpark → act(USR1=H) → q(P,USR1,info=user) → wake-check → sfdresume | parked=1; wake_seq=1; rec_seq=10:user |

### Group N — Native checkpoints (SIGNAL_ABI_NATIVE_IMPL)

| ID | Model | Ops | Expected observations |
|---|---|---|---|
| N01 | py+C | np(T,USR1) → ncheck(mode=0,sel=[]) → nstate | nres=10; npending=- |
| N02 | py+C | np(P,USR1) → ncheck(mode=0,sel=[]) → ncheck(mode=0,sel=[]) → nstate | nres_seq=10;-; nht=- |
| N03 | py+C | np(T,USR1) → ncheck(mode=2,sel=[]) → ncheck(mode=0,sel=[]) → nstate | nres_seq=10;10; npending=- |
| N04 | py+C | np(T,USR1) → np(T,USR2) → ncheck(mode=0,sel=[]) | nres=10,12 |
| N05 | py+C | np(T,USR2) → np(P,USR1) → ncheck(mode=4,sel=USR1,USR2) → ncheck(mode=4,sel=USR1,USR2) → nstate | nres_seq=10;12; npending=-; nht=- |
| N06 | py+C | np(T,USR1) → np(P,USR2) → ncheck(mode=6,sel=USR1,USR2) → nstate | nres=10,12; npending=10; nht=12 |
| N07 | py+C | np(T,USR1) → np(P,USR1) → ncheck(mode=0,sel=[]) → ncheck(mode=0,sel=[]) → nstate | nres_seq=10;10; npending=-; nht=- |
| N08 | py+C | nmask(KILL,STOP,USR1,RT63) → nblock(KILL,STOP,USR1) | natmask=10,63; htblock=10,19 |
| N09 | py+C | nblock(USR1) → np(P,USR1) → ncheck(mode=0,sel=[]) → np(P,USR1) → ntake → nstate | nres=10; ntake=-; nht=10 |
| N10 | py+C | nmask(USR1) → np(P,USR1) → ncheck(mode=4,sel=USR1) → nstate | nres=10; npending=- |

