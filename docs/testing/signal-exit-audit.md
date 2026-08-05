# Signal, stop, and exit protocol audit

`SIGNAL_EXIT_PROTOCOL_AUDIT`

This is the acceptance record for `PROC.md` step 5.  Signals are not genericobject events: signal generation first records persistent pending state underthe signal lock, then a mode-compatible Park wake may consume the currentsequence.  Job-control stop remains outside Park, and remote exit remains apersistent request until the target reaches a safe boundary.

## Park mode and wake reasons

| Wait mode | Ordinary deliverable signal | Fatal signal | Task-exit request | Object event/close |
|---|---:|---:|---:|---:|
| `INTERRUPTIBLE` | wake | wake | wake | wake |
| `KILLABLE` | no wake | wake | wake | wake |
| `UNINTERRUPTIBLE` | no wake | no wake | no wake | wake |

`PROC_WAKE_SIGNAL`, `PROC_WAKE_FATAL_SIGNAL`, and`PROC_WAKE_TASK_EXIT` are distinct from the existing object-close`PROC_WAKE_EXIT`.  This prevents a device or pipe close from being mistakenfor task termination.  `proc_park_prepare_locked()` also checks alreadypending exit, fatal, or deliverable ordinary signals after publishing thetoken, which closes the pending-before-prepare side of the race.

Eventfd and timerfd waits are safely cancelable because their persistentcounter/timer state and tokenized wait entries remain valid after aninterrupted read or write.  They therefore use `INTERRUPTIBLE` and return`-ERESTARTSYS` for task-interrupt wake reasons.  Virtio block requests remain`UNINTERRUPTIBLE`: the in-flight request and stack-owned completion cannot beabandoned until the device has completed it.

## Signal-state serialization

`SIGNAL_STATE_LOCK_CONTRACT` protects:

- shared signal actions, process-pending bits, siginfo presence, and siginfo;
- per-task blocked masks and thread-pending bits;
- temporary-mask, `sigsuspend`, and `sigtimedwait` handoff state.

The only permitted nesting is `proc_lock -> signal_state.lock`.  Signalgeneration takes the signal lock first, releases it, and only then attempts aPark wake or STOPPED transition under `proc_lock`.  Scheduler stop/resume andPark prepare already hold `proc_lock` and may query signal state in thedocumented order.

`SIGNAL_MASK_PARK_PROTOCOL` covers `sigsuspend`: the temporary mask ispublished while holding `proc_lock`, pending state is rechecked, and the Parktoken is prepared before releasing that lock.  `sigtimedwait` similarlypublishes its wait mask before preparing the token.  A matching signal wakesthe waiter even though the signal is blocked for normal handler delivery.

## STOPPED state

A default stop action enters `PROC_STOPPED` only in`proc_sched_stop_current()`.  Before publishing the state, that path rejects aracing pending `SIGCONT`, fatal signal, or exit request.  Ordinary signalsremain pending and cannot resume the task.

`proc_sched_resume_stopped()` is the only resumption path:

- `SIGCONT` resumes and records one continued event;
- a fatal signal resumes without a continued event so it can terminate at the
  next signal boundary;
- a task-exit request resumes without a continued event so the safe-boundary
  exit check can run.

Stop and continue events wake parent waiters, honor `SA_NOCLDSTOP`, and arereported once through `WUNTRACED`/`WCONTINUED` unless `WNOWAIT` is requested.`SIGCONT` is a default-ignore delivery action after its unconditional resumeside effect; it no longer falls through to default termination.

## Remote exit

`REMOTE_EXIT_SAFE_BOUNDARY` implements the following sequence:

1. store the requested code and publish `exit_pending`;
2. try `PROC_WAKE_TASK_EXIT` against the target's current Park sequence;
3. leave an uninterruptible wait asleep until its resource event;
4. explicitly resume a STOPPED target;
5. let normal wait-entry and timer cleanup finish on the target;
6. execute `proc_exit()` from `proc_check_exit_pending()` at a syscall, trap,
   or resumed execution boundary.

There is no fallback that directly changes a BLOCKED task to READY.  If anevent or timeout already won the Park token, `exit_pending` remains observableat the next safe boundary.

## Regression and gate coverage

`proc_stress` verifies:

- a stopped child reports once, remains stopped after an ordinary signal,
  resumes only on `SIGCONT`, then delivers the pending ordinary signal;
- `WCONTINUED` reports the explicit resume and `SIGKILL` terminates a stopped
  child;
- repeated blocked-signal to `sigsuspend` handoffs cannot lose a wake;
- an unblocked signal interrupts eventfd read, while a blocked signal does not.

`make check-signal-exit-boundary` enforces the wake-mode markers, signal-lockownership, removal of `proc_make_ready()` fallbacks, interrupt-reason handling,and the targeted test markers.  `make check-proc-step5-local` adds thedual-architecture debug/release stress matrix; `make check-proc-step5`additionally runs both formal CAgent entries.
