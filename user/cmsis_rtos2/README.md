# A20OS CMSIS-RTOS2 personality

A complete implementation of the Arm CMSIS-RTOS2 API (2.1.3 surface) for
A20OS, positioned where A20OS puts every API: in user space, above an ABI.

```
application (unchanged CMSIS-RTOS2 sources)
        |
   cmsis_os2.h / libcmsis_rtos2      <- this directory
        |
   os_backend: threads, clock, allocator, kernel lock, blocking queue
        |
   Linux ABI (POSIX backend)   or   Native ABI (native backend)
        |
   A20OS kernel mechanisms (scheduler, futex, VM, timers)
```

The kernel does not grow a CMSIS syscall table.  The 80-odd `os*` entry
points are a library; the kernel only sees the handful of primitives the
backend needs, and the trim below makes that set explicit and checkable.

## Layout

| Path | Contents |
|---|---|
| `include/cmsis_os2.h` | complete API declaration (types, priorities, flags) |
| `src/os_kernel.c` | kernel lifecycle, clock, scheduler lock, delay, registry |
| `src/os_thread.c` | threads, thread flags |
| `src/os_sync.c` | mutex, semaphore, event flags |
| `src/os_timer.c` | software timers + timer service thread |
| `src/os_queue.c` | message queues, memory pools |
| `src/backend_posix.c` | POSIX backend: pthreads + futex + `CLOCK_MONOTONIC` |
| `src/backend_native.c` | Native ABI backend for NOMMU/MCU builds |
| `cmsis_rtos2.toml` | **single source of truth** for features and syscalls |
| `tests/test_cmsis.c` | behavioural host self-test |

## What is implemented

* kernel: initialize/start/state/info/version, tick and system-timer clocks,
  lock/unlock/restore, suspend/resume, `osDelay`, `osDelayUntil`
* threads: create/exit/join/detach/terminate/suspend/resume, priorities,
  names, state, count/enumerate, affinity get/set, stack size and high-water
  space
* thread flags: set/get/clear/wait (any/all/no-clear, timeouts)
* timers: one-shot and periodic, start/stop/is-running/delete, callbacks in a
  dedicated service thread (the CMSIS execution context contract)
* mutexes: owner, recursion, timed acquire, delete
* semaphores: counting, max bound, timed acquire, get-count
* event flags: set/clear/get/wait with any/all/no-clear
* message queues: priority-ordered put/get (`msg_prio`, higher first; equal
  priorities FIFO), blocking put/get, capacity/space/reset
* memory pools: fixed blocks, blocking alloc, double-free detection

Objects support `cb_mem`/`cb_size` (static control blocks) and
`stack_mem`/`stack_size` (static thread stacks).

Documented limits:

* `osThreadSuspend/Resume` is cooperative on the POSIX backend (signal based)
  and kernel-level on the Native backend: the target stops at its next
  syscall/IRQ-return safe point (or is published STOPPED directly if it was
  blocked), and resume re-enqueues it.
* `osKernelSuspend()` is a real scheduler-locked WFI on the single-core MCU
  Native backend and returns the elapsed ticks; SMP and hosted backends return
  0 because they cannot suspend the whole machine from one task.
* The POSIX backend keeps the host's pthread scheduling policy; CMSIS
  priorities and priority inheritance are guaranteed by the Native/MCU
  backend, not by a hosted process.
* `osThreadGetAffinity()` returns the kernel's effective affinity mask on
  Native and the host pthread mask on POSIX.  `osThreadSetAffinity()` is
  forwarded to `task_set_sched`; the Native MCU profile is single-core, so
  the useful mask has one bit.
* `osThreadGetStackSpace()` uses a high-water pattern.  The Native backend
  fills every stack it owns; the POSIX backend can only measure a
  caller-provided stack that meets `PTHREAD_STACK_MIN`.  A pthread-owned
  default stack and the initial thread (no stack metadata) report 0.  The
  value is stable for a blocked/terminated thread and best-effort while the
  target is still running.
* The POSIX backend implements `osKernelLock` as a recursive mutex; a
  blocking call inside the lock is still an application error.
* The initial thread installs its own handle at `osKernelInitialize()`
  (`task_self`), so it can be the target of another thread's inheritance
  donation as well as setting/querying its own priority.
* `osThreadSetPriority` returns `osError` while the target is executing a
  donation: the kernel's saved base snapshot cannot be rewritten without
  losing the restore point.

## Real-time model (Native / MCU backend)

A Native-ABI MCU image does not run CMSIS threads on a fair-share scheduler.
`osThreadNew` maps every thread to the kernel's `SCHED_FIFO` class and the
CMSIS priority is the kernel RT priority (1:1, `osPriorityIdle(1)` through
`osPriorityRealtime7(55)`):

* the scheduling attributes travel in `a20_thread_create_args_t` and are
  applied by `proc_sched_set()` **before** the task is published, so a new
  thread never takes a step at the creator's priority;
* a woken higher-priority thread preempts at the syscall/IRQ-return safe
  point, not at the next tick;
* semaphore/mutex release broadcasts; the RT runqueue picks the highest
  waiter, and lower waiters re-check and sleep again;
* mutex inheritance is split at the right boundary: the mutex layer keeps
  the `wait_owner` chain (A waits on B, B waits on C) and walks it on every
  block, donating the waiter's **effective** RT priority to each link; the
  kernel (`proc_sched_rt_donate/restore`) owns the maximum donation, the base
  policy/priority/nice snapshot, the READY-owner re-key through
  `proc_sched_set()`, and the restore on the owner's last mutex release;
* `osKernelLock` is a per-task `sched_lock_depth` checked by the kernel's
  preemption points, not a user-space priority trick.  It defers the
  scheduler, not interrupts; blocking inside the lock is still an
  application error;
* the MCU profile disables the hosted 5%-per-second RT bandwidth reserve, so
  a CMSIS RT thread is not forced to yield 50 ms every second.

Time resolution remains the kernel tick: 100 Hz on Hi3861 (10 ms, the LiteOS
default) and 1 kHz on the QEMU MCU board.  `osDelay(1)`, timeouts and software
timers cannot be finer than that; a sub-tick control loop needs a hardware
timer or a faster tick.

A freestanding target must compile with `OS_NATIVE_TICK_HZ` equal to the
kernel's `ARCH_TIMER_FREQ`.  The in-tree demo derives it from `MCU_TICK_HZ`
(1000 for QEMU, 100 for Hi3861); leaving the default 100 makes
`osKernelGetTickFreq()` disagree with the kernel and stretches every CMSIS
timeout.

`user/tests/liteos-demos/rt.c` exercises this model as experiment 4.5:
creation priority, `osThreadSetPriority`, `osKernelLock` deferral and mutex
inheritance.

## Two backends

| Backend | Use | Primitives |
|---|---|---|
| POSIX | hosted A20OS (Linux ABI), host self-test | pthreads, condvars, `CLOCK_MONOTONIC` |
| Native | NOMMU/MCU builds | `thread_*`, `task_*_sched`, `sched_lock`, `futex_*`, `atomic32`, `vm_alloc`, `clock_get` |

A port to a third ABI implements `os_impl.h`'s backend contract; no `os*`
function needs to change.

## Trim

`cmsis_rtos2.toml` declares profiles.  `tools/cmsis_trim.py` turns a profile
into the two build artefacts and verifies a built kernel against the same
declaration:

```sh
# feature header + Native syscall table subset
python3 tools/cmsis_trim.py emit --profile mcu --out build/trim-mcu

# fail if the kernel links a syscall outside the profile (or misses one)
python3 tools/cmsis_trim.py check --profile mcu --elf build/kernel.elf
```

The generated table overrides the master table with
`-DA20_NATIVE_SYSCALL_TABLE='"syscall_table_mcu.def"'`; the dispatcher
already returns `-NOT_SUPPORTED` for numbers it cannot find, so no second
dispatcher exists.

Profiles:

| Profile | Object families | Syscalls |
|---|---|---|
| `full` | all | all (mirrors the master table) |
| `mcu` | thread, mutex, semaphore, timer, event flags, message queue, suspend/resume, idle, vm_unmap | 19 |
| `mcu-min` | thread, semaphore, timer (no join/terminate/priority control) | 9 |

Measured library text for the Native backend, riscv32imc/ilp32, `-Os`
(`-ffunction-sections`, before link GC): `full` 12.5 KiB, `mcu` 11.8 KiB,
`mcu-min` 10.6 KiB.

A family compiled out keeps its entry points (they return `osErrorResource`),
so an application built against the full header links against every profile;
the trim is about what enters the image, not about forking headers.

## Tests

```sh
make -C user/cmsis_rtos2 test                 # POSIX + Native backends
make -C user/cmsis_rtos2 freestanding PROFILE=mcu-min CC_RISCV32=<triple>-
python3 -m unittest tools.tests.test_cmsis_trim
```

`make test` runs the same behavioural suite twice: once through the POSIX
backend and once through the Native backend with `tests/native_platform_host.c`
implementing the platform calls with pthreads and the Linux futex.  The Native
run prints

```text
osn-used:clock_get,vm_alloc,thread_create,...,atomic32
```

and a unit test additionally checks that the `a20_*` call set in
`src/native_platform_a20.c` is exactly the `mcu` profile's syscall list, so the
backend and the manifest cannot drift.

The suite covers threads, join/count, semaphores, recursive mutexes with four
contenders, event/thread flags, queues (full/empty/reset), pools
(capacity/double free) and periodic/one-shot timers.

## Running as U-mode flat programs

`user/tests/liteos-demos/` links this library (Native backend) into a
static-PIE image that the RV32 M-mode kernel loads and runs in U-mode:

```sh
tools/a20 test qemu-riscv32-mcu
# [4.1] PASS steps=5/5
# [4.2] PASS taken=5 produced=5
# [4.3] PASS once=1 periodic=12
# [4.4] PASS irq=5
# [4.5] PASS create=1 setprio=1 lock=1 inherit=1 chain=1 init=1
# [4.6] PASS suspend=1 resume=1
# [4.7] PASS slept=1 elapsed=1
# [4.8] PASS delay=<cycles> syscall=<cycles>
# [4.9] PASS good=1 entered=1 survived=0
# [DEMO] experiments 4.1-4.9: PASS
```

Experiments 4.1-4.3 include only `cmsis_os2.h`.  Experiment 4.4 adds the
Hi3861 vendor surface from `user/port/hi3861_hal/`; nothing under this
directory includes or names those symbols.  The vendor calls live in their own
syscall range (`0x7F00..`) handled by `kernel/arch/riscv32/mcu/vendor_hi.c`,
so the generated Native table and the trim check remain the standard-only
surface.
