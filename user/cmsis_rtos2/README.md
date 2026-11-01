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
  names, state, count/enumerate, affinity, stack size/space
* thread flags: set/get/clear/wait (any/all/no-clear, timeouts)
* timers: one-shot and periodic, start/stop/is-running/delete, callbacks in a
  dedicated service thread (the CMSIS execution context contract)
* mutexes: owner, recursion, timed acquire, delete
* semaphores: counting, max bound, timed acquire, get-count
* event flags: set/clear/get/wait with any/all/no-clear
* message queues: FIFO, blocking put/get, capacity/space/reset
* memory pools: fixed blocks, blocking alloc, double-free detection

Objects support `cb_mem`/`cb_size` (static control blocks) and
`stack_mem`/`stack_size` (static thread stacks).

Documented limits:

* `osThreadSuspend/Resume` is cooperative on the POSIX backend (signal based)
  and not supported by the Native backend; it returns `osError` there.
* The POSIX backend cannot honour `osKernelLock` for code that blocks while
  locked; the lock is a recursive mutex, so the API contract holds but a
  blocking call inside a locked region is the usual application error.
* Message queues are FIFO; `msg_prio` is accepted and ignored.
* Priority inheritance (`osMutexPrioInherit`) depends on the backend's
  scheduler; the flag is stored and the POSIX backend does not enable PI.

## Two backends

| Backend | Use | Primitives |
|---|---|---|
| POSIX | hosted A20OS (Linux ABI), host self-test | pthreads, condvars, `CLOCK_MONOTONIC` |
| Native | NOMMU/MCU builds | `thread_*`, `futex_*`, `atomic32`, `vm_alloc`, `clock_get` |

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
| `mcu` | thread, mutex, semaphore, timer, event flags, message queue | 12 |
| `mcu-min` | thread, semaphore, timer (no join/terminate/priority control) | 9 |

Measured library text for the Native backend, riscv32imc/ilp32, `-Os`
(`-ffunction-sections`, before link GC): `full` 11.7 KiB, `mcu` 11.0 KiB,
`mcu-min` 9.8 KiB.

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
# [DEMO] experiments 4.1-4.4: PASS
```

Experiments 4.1-4.3 include only `cmsis_os2.h`.  Experiment 4.4 adds the
Hi3861 vendor surface from `user/port/hi3861_hal/`; nothing under this
directory includes or names those symbols.  The vendor calls live in their own
syscall range (`0x7F00..`) handled by `kernel/arch/riscv32/mcu/vendor_hi.c`,
so the generated Native table and the trim check remain the standard-only
surface.
