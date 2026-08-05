# SMP runqueue and preemption audit

`SMP_RUNQUEUE_PREEMPT_AUDIT`

This document closes PROC.md step 7 without changing the scheduler's existingpriority classes or architecture context layouts.

## Ownership and lock order

`proc_lock` remains the coarse scheduler/lifecycle serialization boundary forenqueue, migration, Park/Wake, switch publication and completion, exit andreap. Step 8 narrows one hot-path exception: local pick changes`on_rq -> dispatching` and publishes `owner_cpu` under only that CPU's runqueuelock, then releases it before switch publication acquires `proc_lock`.

Cross-CPU migration uses `sched_runq_requeue_locked()`:

1. acquire `proc_lock`;
2. acquire the source and destination runqueue locks in ascending CPU-number
   order;
3. unlink the task from the source queue and publish `on_rq = 0`;
4. change `cpu_id`;
5. link the task to the destination queue and publish `on_rq = 1`;
6. release runqueue locks in reverse order, then release `proc_lock`.

The runqueue-owned task reference is transferred through the operation. Thereis no put/get gap and no interval observable to a picker in which a READY taskis owned by neither queue. A same-CPU policy requeue follows the same`on_rq -> off-rq -> on_rq` rule while holding one runqueue lock.

`cpu_id` is initialized only for unpublished/off-runqueue tasks. Once a task isqueued, it changes only in the protected off-runqueue interval above.

## Persistent reschedule requests

Each CPU has cacheline-separated scheduler state containing a persistent`need_resched` flag and diagnostic counters. A request:

- publishes `need_resched = 1` with release ordering;
- sends at most one IPI while the flag remains pending;
- never relies on the IPI itself as the stored scheduling decision.

The target IPI handler acknowledges the hardware notification and incrementsthe acknowledgement counter. It does not call `sched()`, `proc_yield()`, orchange task/runqueue ownership.

The pending flag is consumed only when `sched()` is entered from:

- the common user trap/syscall return safe point;
- a timer return which first publishes a timeslice request;
- an explicit kernel scheduling point, including the idle loop.

If a new request races after consumption, the release-store leaves the flag setfor the next safe point. IPI acknowledgement never clears it.

## Wakeup and priority policy

Every remote queued wake publishes a request for its target CPU. Local wakeupsrequest preemption only when the woken task outranks the current task:

- an RT task outranks a non-RT task;
- among RT tasks, the larger existing RT priority wins;
- among non-RT tasks, the existing lower scheduler level wins;
- any runnable task outranks idle.

This only decides whether to request a safe-point reschedule. Queue selection,FIFO/RR behavior, aging, nice values, and the existing priority strategy areunchanged. At a safe point, a yielding task remains the CPU owner until switchcompletion. If it still strictly outranks the selected queued task, thescheduler reverses the unpublished `on_rq -> dispatching` transfer and retainsthe current task. Equal-priority tasks still switch, preserving yield andround-robin progress; a lower-priority task cannot run in the ownership gap.

## Diagnostics and regression

`/proc/a20/task_lifetime` exposes:

- `runqueue_migrations`;
- request, priority-request, IPI sent/acknowledged, consumed, and pending
  counters;
- `scheduler_violations`.

`sched_stress` creates a remote CPU RT hog, queues a lower RT task behind it,then atomically migrates that queued task to the parent CPU. It verifies:

- the task executes only on the destination CPU;
- at least one cross-runqueue migration occurred;
- remote requests caused IPI send and acknowledgement;
- a higher-priority destination wake requested preemption;
- a safe point consumed the request;
- scheduler violations remain zero.

The test reports a one-CPU skip and runs the full protocol on the eight-CPUdebug/release matrix. The cumulative lifetime stress continues to check that atask occupies at most one runqueue/CPU and that all earlier Park/Wake, signal,timeout, process, VFS, socket, futex, and reference invariants remain green.
