# Task lifetime ownership audit

`TASK_LIFETIME_OWNERSHIP_AUDIT`

This audit is the acceptance record for `PROC.md` step 3.5. It covers theownership model introduced by the Park/Wake, CPU ownership, and task lifetimechanges. The diagnostic surface is read-only at`/proc/a20/task_lifetime`; it does not alter scheduling decisions.

## Ownership transfer table

| Owner | Reference acquisition | Transfer or release |
|---|---|---|
| Allocation/global task list | `proc_task_alloc_storage()` creates the allocation reference before `proc_alloc_task_slot()` publishes the task | `proc_destroy_task()` marks `destroy_started`, removes scheduler/global reachability, then drops the allocation reference |
| Static idle task | `proc_task_init_idle_state()` installs one permanent base reference | Never released; `/proc/a20/task_lifetime` checks every current static task still has its base reference in addition to CPU ownership |
| PID table | `proc_pid_register()` calls `proc_get()` before publication | `proc_pid_unregister()` removes the hash/bitmap entry, then calls `proc_put()` |
| Runqueue | `proc_runq_enqueue_locked()` calls `proc_get()` before setting `on_rq` | `proc_runq_pick_local()` transfers the same reference to `dispatching` under the local runqueue lock; `proc_runq_remove_locked()` releases it |
| Dispatching/current CPU | A picked task keeps the runqueue reference while `dispatching`; `proc_set_current()` acquires the current-slot reference | `context_switch()` releases the dispatch reference; `proc_switch_complete()` releases the outgoing current/switching reference after the old stack is inactive |
| Wait entry | `wait_queue_link()` and `futex_waiter_alloc()` call `proc_get()` before publication | unlink/removal releases it, or collection transfers it to a wake batch |
| Wake batch | `proc_wake_q_add()` receives the wait-entry reference without an extra get | `proc_wake_q_flush()` calls `proc_try_wake_locked()` and then `proc_put()` exactly once |
| Timeout heap | `proc_wait_timer_register_locked()` calls `proc_get()` before heap insertion | cancel and expiry remove the heap item before `proc_put()` |
| PID lookup caller | `proc_find_get()` increments under `pid_lock` | Every successful external lookup is paired with `proc_put()`; helpers which return a referenced task document that ownership in their caller |

## Static audit

- No kernel call site uses a bare `proc_find()`.
- `proc_find_get()` call sites in process, signal, procfs, VFS, cgroup, OOM,
  device, Linux ABI, and native ABI paths were checked for a matching`proc_put()` on success and error exits. `sys_sched` lookup helpers transferthe returned reference to their syscall caller, which releases it.
- Every task pointer stored across a lock boundary by PID, runqueue, current
  CPU, wait queue, futex waiter, wake batch, or timeout heap owns or receives areference.
- `on_rq`, `dispatching`, and `on_cpu` are mutually exclusive. The runqueue
  reference is transferred, not reacquired, at `on_rq -> dispatching`;current-slot ownership is released only after switch completion.
- Final resource destruction is reachable only from the last `proc_put()` and
  requires both a dynamic task and `destroy_started`.
- A duplicate destroy, failed live-task get, reference underflow, or final put
  of a live/static task increments a monotonic diagnostic error before theunsafe path is stopped.

`make check-task-lifetime-boundary` protects the static markers and the ban onbare PID lookup. `make check-proc-step35-local` runs the dual-architecturedebug/release smoke and race matrix; `make check-proc-step35` additionallyruns both formal CAgent evaluations.

## Runtime closure criteria

`lifetime_stress` runs the scheduler, futex, process, I/O-event, and VFS stressprograms, followed by repeated batches of:

- `fork/exit/wait4`;
- `signal/exit`;
- timeout/exit;
- futex wake/exit.

It samples the diagnostic surface before and after the run and requires taskobjects, total references, listed tasks/references, PID entries, wait entries,wake entries, and timeout entries to return to the same baseline. It alsorequires `lifetime_errors: 0`. The host-side target additionally requires everystress PASS marker, a zero QEMU status, and the normal power-off marker.
