# Process lock split and scheduler hot-path audit

`PROCESS_LOCK_SPLIT_AUDIT`

This document closes PROC.md step 8 after the Park/Wake, task lifetime,timeout, SMP migration, and persistent preemption protocols have passed theirdual-architecture stress matrices. The change is deliberately limited to themeasured scheduler hot path; it does not weaken lifecycle serialization merelyto increase the number of locks.

## Existing split boundaries

The audit found that several domains named by PROC.md were already split:

| Domain | Protecting lock | Step 8 decision |
|---|---|---|
| PID bitmap and hash | `pid_lock` | keep separate |
| signal actions and pending state | `signal_state.lock` | keep separate; `proc_lock -> signal_state.lock` when both are required |
| runqueue links and membership | one `runq.lock` per CPU | remove global serialization from local pick |
| task list, parent/wait and reap | `proc_lock` | keep coarse until parent/reap contention is measured |
| Park token and scheduler state outside local pick | `proc_lock` | keep coarse because wake, timeout, exit and switch completion share one state transition |

The remaining structural scheduler bottleneck was local queue selection:every CPU acquired `proc_lock` before scanning and aging its own runqueue.That made independent per-CPU queues serialize even though their links andmembership were already protected by distinct locks.

## Local-pick split

`proc_runq_pick_local()` now performs only:

```text
lock(this_cpu.runqueue)
    age this CPU's queues
    select a task
    on_rq -> dispatching
    publish owner_cpu
unlock(this_cpu.runqueue)
```

It never acquires `proc_lock`. The caller acquires `proc_lock` only after therunqueue lock is released, to compare the selected task with a still-ownedhigher-priority current task and to publish `dispatching -> on_cpu` during thecontext switch.

The split preserves these linearization points:

- enqueue, removal, policy requeue and migration still use
  `proc_lock -> runqueue lock`;
- local pick transfers the runqueue-owned task reference to dispatch ownership
  under the local runqueue lock;
- unpick uses `proc_lock -> runqueue lock` and transfers that same reference
  back without a put/get gap;
- switch publication and switch completion remain under `proc_lock`;
- Park, wake, timeout, exit, parent/wait and reap semantics are unchanged.

A local picker never acquires `proc_lock` while holding a runqueue lock.Therefore it cannot invert the global order. A lifecycle observer holding`proc_lock` may wait for a picker to release a runqueue lock; the pickerfinishes its bounded local operation without waiting for that observer.

## Deferred splits

Parent/wait and task Park state remain under `proc_lock`. Splitting them wouldrequire a new proof for exit/wake/reap and timeout/wake/finish races, while thecurrent diagnostics do not identify either domain as the runqueue-selectionbottleneck. They should only be split after a workload records sustainedcontention in those paths and a dedicated ownership test exists.

No architecture context, register layout, priority class, CPU-selection policy,or IPI behavior changes in this step.

## Diagnostics and acceptance

`/proc/a20/task_lifetime` now reports:

- `runqueue_local_picks`;
- `runqueue_empty_picks`;
- aggregate per-CPU `runqueue_lock_acquires`;
- observed `runqueue_lock_contentions`;
- `runqueue_parallel_pick_peak`.

The contention counter is diagnostic: it records that the lock was alreadyowned when acquisition began and may conservatively undercount races. Theparallel peak records overlapping local-pick attempts and is not required toexceed one on a particular emulator run.

`sched_stress` forces 256 explicit scheduling points and verifies that localpick and runqueue-lock counters advance, contention never exceeds acquisition,the parallel peak is initialized, and scheduler violations remain zero. Theexisting SMP migration/preemption test and the cumulative lifetime, futex,process, signal, timeout, I/O, VFS and socket tests continue to run unchanged.
