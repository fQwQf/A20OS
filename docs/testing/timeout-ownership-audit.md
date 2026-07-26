# Timeout ownership audit

`TIMEOUT_OWNERSHIP_AUDIT` records the scheduler step-six contract for Park
deadlines. The implementation remains a tokenized deadline min-heap; this step
does not introduce a second timer wheel.

## Ownership

Each heap entry owns exactly one task reference and records the immutable
triple `(task, wait_seq, deadline)`. Registration acquires that reference.
Exactly one of cancellation or expiry removes the entry under `proc_lock` and
releases the reference. Heap removal clears the task's heap index before an
entry can be moved into the vacated slot.

The task's Park deadline uses `wait_deadline` and `wake_time`. POSIX
`alarm`/`ITIMER_REAL` uses the separate `alarm_expire` and
`itimer_real_interval` fields. Alarm delivery does not create or remove a Park
heap entry.

## Linearization and stale events

- A task may own at most one heap index. Registering a second entry for the
  same task is rejected; registration never cancels an existing token.
- Event, signal, exit, cancellation, and timeout removal serialize under
  `proc_lock`.
- Expiry removes the heap entry first, then calls
  `proc_try_wake_locked(task, wait_seq, PROC_WAKE_TIMEOUT)`. A late expiry
  therefore cannot mutate a later `wait_seq`.
- The timer callback touches neither object locks nor stack-resident wait
  entries. Object wait queues are unlinked by the resumed waiter.
- Expired Park deadlines are woken directly through the scheduler state
  machine. They do not pass through a bounded wake array, so there is no
  `READY`-without-runqueue overflow case.

## Capacity failure

The production default is 8192 entries (64 on MCU). A test build may select a
smaller real heap with `WAIT_TIMER_HEAP_MAX`; this changes the compiled array,
not a synthetic limit layered above it.

When the heap is full, registration returns
`PROC_PARK_PREPARE_TIMEOUT_CAPACITY`. `proc_park_wait()` exposes that as
`PROC_WAKE_TIMEOUT_CAPACITY`, and Linux timed syscalls map it to `EAGAIN`.
Untimed waits are unaffected. Internal sleeps retry after yielding instead of
returning early.

`/proc/a20/task_lifetime` reports the compiled capacity, current entry count,
full-registration failures, duplicate rejections, stale expirations, and heap
invariant violations. Heap violations contribute to `lifetime_errors`.

## Runtime coverage

The step-six matrix compiles a real 64-entry heap and runs on both RISC-V64 and
LoongArch64 in debug single-core and release eight-core configurations.
`lifetime_stress` fills the heap to 63, 64, and an attempted 65 entries. It
checks exact occupancy, requires the 65th `nanosleep` to return `EAGAIN`, kills
and reaps every filler, and requires the heap and task-reference counts to
return to baseline.

`futex_stress` also wakes one timed wait early and immediately starts a later
wait on the same task. The second wait must last until its own deadline,
proving that a cancelled old deadline cannot wake a new token.
