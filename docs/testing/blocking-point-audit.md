# Blocking-point protocol audit

`BLOCKING_POINT_PROTOCOL_AUDIT`

This is the acceptance record for `PROC.md` step 4.  A blocking operation is
closed only when its persistent condition, condition lock, Park token,
asynchronous task reference, one-shot wake winner, and resume cleanup are all
identifiable.  Signal/stop/exit policy and timeout-heap capacity remain the
separately scoped work of steps 5 and 6; their compatibility paths are listed
below instead of being silently treated as complete.

## Audited blocking families

| Family | Persistent condition and lock | Wait publication and cleanup |
|---|---|---|
| mutex and completion | `locked/owner` under `mutex.lock`; `done` under `completion.lock` | The current task prepares before the object lock, rechecks under it, links `(task ref, seq)`, commits after unlock, then unlinks and finishes.  Unlock/complete detach wait entries before flushing wake tokens. |
| wait4 | child state, parent relation, and `waiting_for_child` under `proc_lock` | The child condition is persistent under `proc_lock`; `proc_park_prepare_locked()` publishes the token in the same critical section.  Child exit changes state and wins the token under that lock. |
| vfork | `CLONE_VFORK`, `vfork_waiting`, and child exit/exec completion under `proc_lock` | The completion follows the normal completion protocol.  The parent now owns an explicit child reference from publication through completion unlink, so auto-reap cannot free the embedded completion. |
| pipe and PTY | ring occupancy, endpoint-open counts, and hangup state under the pipe/PTY lock | Read/write recheck under the object lock, link a tokenized wait entry, and unlink on every return.  Data/open-state changes detach waiters under the same lock and flush after unlock. |
| eventfd and timerfd | counter/space or timer expiration under the object lock | The object state is persistent.  Wait entries own `(task ref, seq)` and timer deadlines use the same Park token; wake, timeout, and cancellation converge at `proc_try_wake()`. |
| Native channel | queue and peer-closed state under endpoint locks | Send/receive are nonblocking and return `-EAGAIN`; readiness is persisted in the endpoint and exported through the Native event queue.  The channel does not store a bare task pointer. |
| SysV semaphore | semaphore values, removal state, and operation feasibility under `g_sem_lock` | A single token is prepared outside `g_sem_lock`, linked to the set wait queue after a locked recheck, and always canceled/committed, unlinked, and finished before reuse or return. |
| file locks | lock table plus `g_file_lock_generation` under `g_file_lock_table_lock` | A generation closes the condition-check/link window.  Wait entries carry `(task ref, seq)` and unlock/table changes detach before wake flushing. |
| sockets and network bottom-half | accept queues, receive data/EOF/error, send space, and connect state under `g_net_lock` | Accept/read/write/connect prepare outside the network lock, recheck and link under it, then clean up after commit.  Bottom-half callbacks update persistent state and collect task references under `g_net_lock`, but call Park wake only after releasing it. |
| futex | user word translation under `mm->lock`, waiter bucket under `g_futex_lock` | `FUTEX_WAIT` faults in and checks the word once, prepares a token, then takes `mm->lock -> g_futex_lock`, reloads the user word, and links `(task ref, seq)` in the same bucket critical section.  A matching wake cannot pass the bucket lock between the second check and link.  Timeout, signal, event, and cleanup all use the same sequence. |
| virtio-blk | request completion and in-flight state under `virtio_blk_instance.lock` | Each request owns a wait queue; IRQ/poll completion persists `done`, detaches waiters under the device lock, and flushes after unlock.  Deadline and event share one token. |
| UART and input | RX/event ring state under `rx_lock` or device instance lock | The IRQ path persists input before collecting tokenized wait entries.  Waiters register before the locked ring rescan, then unlink and finish on event/signal/cancel. |

All normal wait-queue entries, futex waiters, wake-batch entries, and timeout
heap entries contain both an owned task reference and the Park sequence.  A
waker first detaches or transfers that reference, drops the condition lock,
attempts the one-shot sequence transition, and releases the reference exactly
once.

## Explicit compatibility whitelist

The following paths are intentionally visible to the static gate:

1. `proc_task_alloc_storage()` initializes a newly allocated, unpublished task
   as `PROC_BLOCKED`.  It has no active Park token and becomes runnable only at
   one of the whitelisted task-publication call sites.
2. `proc_park_commit()` is the only live-task path that writes
   `PROC_BLOCKED`.
3. `proc_make_ready()` remains for new task publication, current-task yield,
   and cgroup unthrottle.  Its Park branch delegates to
   `proc_try_wake_locked(task, wait_seq, EVENT)`, so it cannot bypass a live
   token.
4. The three signal fallback call sites and the STOPPED fallback in
   `proc_force_exit()` are step-5 debt.  They are not new blocking APIs:
   a Parked task first goes through sequence-checked SIGNAL/EXIT wake; only the
   legacy non-Park STOPPED path reaches generic READY publication.
5. Linux `poll`, `select`, and `epoll` currently use bounded periodic
   `proc_park_wait()` deadlines and rescan persistent readiness after every
   quantum.  They store no asynchronous task pointer and cannot lose
   correctness, but they are not yet event-driven multi-object subscriptions.
   This bounded compatibility wrapper is accepted for step 4 and retained as
   performance/latency debt, not represented as a completed VFS poll-hook
   design.

There is no remaining `proc_block_until()` call or implementation.  No module
outside task initialization and the scheduler writes `on_rq`, `cpu_id`,
`rq_next`, or `rq_prev`.

## Regression and gate coverage

`futex_stress` runs an unrelated-futex wake storm while a different futex must
reach its deadline.  This rejects the removed global wake-generation shortcut,
which could report a spurious successful wait.  `proc_stress` repeatedly
auto-reaps `vfork` children while the parent is blocked on the child-embedded
completion.  `lifetime_stress` also runs scheduler, futex, process, I/O-event,
VFS, and socket stress before verifying that task, PID, wait, wake, timeout,
and reference counts return to baseline.

`make check-blocking-point-boundary` enforces the old-API ban, direct-state and
runqueue-field boundaries, the finite `proc_make_ready()` whitelist, tokenized
asynchronous wait structures, and the targeted regression markers.
`make check-proc-step4-local` adds the dual-architecture debug/release runtime
matrix; `make check-proc-step4` additionally runs both formal CAgent entries.
