# A20OS Improvement TODO

This document records the current engineering bottlenecks and remaining improvement work for A20OS. It is based on the present code and documentation, not future design intent.

## P0: Concurrency and SMP Readiness

- [x] Replace the current `NR_CPUS != 1` build-time block with a passing SMP validation gate before enabling multi-core builds by default.
  - Evidence: `Makefile` blocks SMP unless `ALLOW_UNVERIFIED_SMP=1`; `kernel/proc/sched.c` lists scheduler/MM/VFS concurrency prerequisites.
  - Done when: `make NR_CPUS=2 check-concurrency-foundation` passes without the override flag.
- [x] Audit every task state transition for the `proc_lock -> runq_lock` ordering contract.
  - Evidence: `kernel/include/core/lock.h` defines global lock order; `kernel/proc/sched.c` depends on per-CPU runqueue state.
  - Done when: fork, exec, exit, wait, signal wake, timer wake, futex wake, and wait-queue wake are covered by runtime stress tests.
- [x] Convert remaining MM paths that rely on single-threaded execution to hold `mm->lock` for VMA and page-table mutations.
  - Evidence: `kernel/include/mm/vm.h` states some paths still rely on single-threaded execution or narrower local locking.
  - Done when: `make check-mm-lock-model` includes behavioral tests for concurrent mmap, munmap, fault, fork COW, and exit teardown.
- [x] Add runtime VFS concurrency stress for close/read/write, dup/close_range, rename/unlink/open, symlink loops, and mount/unmount.
  - Evidence: `kernel/fs/vfs.c` describes runtime expansion as future work in the VFS concurrency smoke matrix.
  - Done when: `make check-vfs-abstraction` fails on regressions in those races, not just on missing documentation markers.

## P0: Linux ABI Correctness

- [x] Promote Linux syscall areas from `partial` to `full` only after syscall-group smoke tests and edge-case tests exist.
  - Evidence: `kernel/abi/linux/syscall_coverage.md` marks fd I/O, paths, process lifecycle, signals, MM, futex, poll, sockets, and timers as partial.
  - Done when: each upgraded area has tests listed next to the coverage table entry.
- [x] Replace scheduler policy and affinity approximations with behavior matching supported Linux semantics.
  - Evidence: `kernel/abi/linux/syscall_coverage.md` marks scheduler APIs as compatibility approximations.
  - Done when: sched policy, priority, affinity, and cgroup cpuset behavior are covered by LTP-style tests.
- [x] Complete advanced futex operations and memory-ordering edge semantics.
  - Evidence: `kernel/abi/linux/syscall_coverage.md` marks futex as partial; `kernel/abi/linux/sys_futex.c` still has unsupported paths.
  - Done when: basic, requeue, private/shared, timeout, and robust-list cases are covered.
- [x] Decide which explicit `-ENOSYS` Linux syscall placeholders remain out of scope and which should be implemented.
  - Evidence: `kernel/abi/linux/syscall_table.def` contains fanotify, signalfd, AIO, module, userfaultfd, perf, and arch-prctl placeholders.
  - Done when: every placeholder has a documented owner decision: implement, keep stub, or remove from claimed compatibility scope.

## P0: MM, Page Cache, and File Mapping

- [x] Add a dirty-page/writeback owner for `MAP_SHARED` file mappings.
  - Evidence: `kernel/include/mm/vm.h` says `MAP_SHARED` writeback/truncate coherence is incomplete.
  - Done when: shared mmap writes become visible through read, fsync, truncate, fork, and remap paths according to the supported semantics.
- [ ] Strengthen page cache eviction and coherence tests across file read/write, mmap fault, truncation, and reclaim.
  - Evidence: `kernel/mm/fault.c` and `kernel/fs/page_cache.c` expose current page-cache limits and unsupported paths.
  - Done when: page-cache tests run under memory pressure and catch stale data or use-after-free regressions.
- [ ] Validate huge-page demotion and COW behavior under fork, mprotect, munmap, and OOM reclaim.
  - Evidence: `kernel/mm/vm.c` implements huge-page demotion and COW clone paths; the lock model requires careful TLB/refcount ordering.
  - Done when: regression tests cover mixed huge/small mappings, write faults, and TLB-flush-sensitive cases.
- [x] Make OOM reclaim policy observable and testable.
  - Evidence: `kernel/include/mm/vm.h` states reclaim must not free frames reachable from task MM, page cache, VMO, or Native handles.
  - Done when: OOM tests prove safe kill/reclaim behavior instead of only logging allocation failures.

## P1: I/O Progress and Networking

- [ ] Replace scheduler/idle polling progress with event-driven wakeups where block and network devices can signal completion.
  - Evidence: `docs/external-dependencies.md` describes polling-based lwIP progress; `kernel/drivers/block/virtio_blk.c` notes a future interrupt wake path.
  - Done when: block and network progress no longer depend on generic hot-path polling for normal operation.
- [ ] Reduce `g_lwip_lock` contention and document lock-safe entry points for all socket paths.
  - Evidence: `kernel/net/lwip_stack.c` serializes lwIP core state behind a global lock; `kernel/include/core/lock.h` restricts calls under lwIP locks.
  - Done when: socket send/recv/connect/listen/accept tests run concurrently without lock-order warnings or starvation.
- [ ] Replace QEMU-only network address defaults with board/network configuration plumbing.
  - Evidence: `docs/external-dependencies.md` says `10.0.2.15`, `10.0.2.2`, and `10.0.2.3` are development defaults only.
  - Done when: real boards or non-QEMU backends configure IP, gateway, and DNS without hard-coded QEMU assumptions.
- [ ] Expand network smoke coverage beyond wget success.
  - Evidence: `docs/external-dependencies.md` says TLSe/wget are not proof of a complete modern HTTPS stack.
  - Done when: DNS, UDP, TCP, ICMP, AF_UNIX, AF_ALG, timeout, partial I/O, and error-path tests are separate gates.

## P1: VFS and Filesystem Semantics

- [ ] Tighten path resolution, symlink, permission, mount, and filesystem-specific Linux edge semantics.
  - Evidence: `kernel/abi/linux/syscall_coverage.md` marks path and metadata as partial and needing cleanup.
  - Done when: openat/openat2, renameat2, link/symlink, xattr, chmod/chown, statx, mount, umount, and chroot have focused tests.
- [x] Refactor the large VFS implementation into smaller ownership, path, mount, fd, and syscall-facing units.
  - Evidence: `kernel/fs/vfs.c` is a large central implementation that carries path resolution, open/close, mount, init, and compatibility behavior.
  - Done when: each unit has a narrow header contract and subsystem-specific tests.
- [ ] Remove hard-coded runtime filesystem initialization from generic VFS paths where possible.
  - Evidence: `kernel/fs/vfs.c` initializes default virtual files and environment-like content during VFS bringup.
  - Done when: policy files move to init/userland image construction or a declarative boot filesystem manifest.
- [ ] Define a clear consistency model for FAT32, ext4, ramfs, devfs, procfs, sysfs, pipe, and anonfd operations.
  - Evidence: `kernel/fs/` contains multiple filesystem backends with Linux ABI entry points marked partial.
  - Done when: backend capability differences are documented and surfaced through tests instead of implicit `-ENOSYS` returns.

## P1: Native ABI Completion and Maintainability

- [x] Split oversized Native phase-2 syscall implementation into subsystem-owned files.
  - Evidence: `kernel/abi/native/sys_phase2.c` contains broad memory, IPC, security, debug, and system functionality.
  - Done when: Native syscall files mirror subsystem boundaries and each file owns a narrow syscall range.
- [ ] Complete or explicitly scope down Native debug semantics.
  - Evidence: `kernel/abi/native/sys_phase2.c` documents debug calls as limited compatibility implementations without full stop/resume/watchpoint behavior.
  - Done when: debug handle behavior is either fully implemented and tested or documented as intentionally limited in the Native ABI docs.
- [ ] Finish handle transfer, partial-delivery, temporal-rights, and label consistency tests.
  - Evidence: `kernel/abi/native/handle_table.h` and `kernel/abi/native/handle_table.c` reference partial-delivery and capability consistency gates.
  - Done when: Native IPC tests cover successful transfer, failed transfer, revoked rights, expired rights, and label denial.
- [ ] Reconcile Native ABI documentation with active userland runtime implementation.
  - Evidence: `docs/native-abi/00-overview.md` describes completed Native ABI work; active userland also relies heavily on Linux ABI musl builds.
  - Done when: docs distinguish active Linux-musl userland, `liba20rt`, `liba20c`, archived A20 syscall bridge code, and future Native POSIX shim work.

## P1: Driver and Device Model

- [x] Replace fixed-size driver/device/bus registries with dynamically sized or capacity-checked registries that report structured errors.
  - Evidence: `kernel/drivers/core/driver_core.c` uses bounded static registries for bringup.
  - Done when: registry exhaustion is tested and does not silently lose devices or drivers.
- [ ] Add hotplug and remove-path lifecycle tests before treating the driver model as general-purpose.
  - Evidence: `kernel/drivers/core/driver_core.c` has probe/remove paths but the model is primarily built-in bringup oriented.
  - Done when: bind, probe failure, remove, re-probe, and resource cleanup have tests.
- [ ] Move device-specific lock ordering into driver docs next to each private lock.
  - Evidence: `kernel/include/core/lock.h` requires new locks to fit the global order or document a local order.
  - Done when: virtio-blk, virtio-net, UART, PTY, loop, SDIO, and platform NICs each document their private lock rules.

## P2: Test Gates and Tooling

- [ ] Convert static `rg`-style architecture gates into behavior tests where a behavior can be executed under QEMU.
  - Evidence: `docs/testing-gates.md` defines repeatable gates, but several current checks validate documentation markers rather than runtime behavior.
  - Done when: every architecture-debt TODO has either a runtime test, a build matrix test, or a justified static-only check.
- [ ] Add LTP-style grouped smoke tests for every Linux ABI coverage area before claiming broader compatibility.
  - Evidence: `kernel/abi/linux/syscall_coverage.md` says each syscall group needs smoke tests before level upgrades.
  - Done when: coverage table generation includes test target names and last-known status.
- [ ] Expand Native ABI tests beyond minimal process startup and libc smoke.
  - Evidence: `docs/testing-gates.md` names `native-minimal`, `native-test`, and `user/tests/test_liba20c.c` as Native coverage.
  - Done when: Native handle, VMO/VMAR, channel, event queue, timer, task, debug, and rights tests run in CI-like targets.
- [ ] Add stress tests for memory pressure, fork/exec churn, fd churn, filesystem churn, network churn, and process reaping.
  - Evidence: current smoke targets prove basic operation but not long-run stability or race behavior.
  - Done when: stress targets run with bounded timeouts and capture kernel logs on failure.

## P2: Repository Hygiene and Dependency Boundaries

- [x] Remove or quarantine patch-artifact files from active source directories.
  - Evidence: files such as `kernel/proc/fork.c.orig`, `kernel/proc/fork.c.rej`, and `kernel/abi/linux/sys_futex.c.orig` appear in active trees.
  - Done when: active source directories contain only build inputs, documentation, or intentionally tracked fixtures.
- [ ] Keep vendored code out of first-party quality claims and tests unless the test is explicitly an integration test.
  - Evidence: `docs/external-dependencies.md` separates lwIP, musl, sbase, mksh, TLSe, and wget roles from A20 integration work.
  - Done when: TODOs and status docs consistently credit A20 for integration, not upstream TCP/IP, libc, shell, or coreutils implementations.
- [ ] Add an external dependency upgrade checklist target that runs the relevant smoke groups automatically.
  - Evidence: `docs/external-dependencies.md` requires Linux syscall smoke, shell smoke, and coreutils smoke after changing imported userland.
  - Done when: changing musl, sbase, mksh, lwIP, TLSe, or wget has a documented command sequence and expected artifacts.
- [ ] Document which archived userland code is historical reference and which is expected to be revived.
  - Evidence: `user/archive/` contains A20 syscall bridges, pthread/mutex code, tests, and older coreutils with TODOs and ENOSYS stubs.
  - Done when: archived paths are excluded from active status claims or moved back with owners and tests.

## Verification Environment Notes

- `aarch64-linux-gnu-gcc` is not installed in this environment, so `check-aarch64-bringup`, `check-aarch64-user`, and `smoke-aarch64` cannot run here.
- `qemu-system-x86_64` is not installed, so `smoke-x86_64` cannot run here, but the x86_64 kernel build (`ARCH=x86_64 ABI=both BRINGUP=1 kernel-only`) succeeds.
- All other static check gates pass: `check-abi-boundary`, `check-mm-lock-model`, `check-vfs-abstraction`, `check-driver-core-model`, `check-io-progress-model`, `check-external-dependency-boundary`, `check-doc-test-gates`, `check-final-definition`, `check-concurrency-foundation`, `check-abi-smoke-gate`, `check-doc-drift`.
- Smoke tests that run successfully on this host (riscv64/loongarch64 bringups timeout by design in bringup mode): `smoke-riscv64`, `smoke-loongarch64`, `smoke-abi-linux`, `smoke-proc-a20`, `smoke-vfs-stress`, `smoke-mm-stress`, `smoke-proc-stress`, `smoke-sched-stress`, `smoke-futex-stress`.
- Running multiple smoke targets concurrently (e.g. `make -j16 smoke-mm-stress smoke-sched-stress`) races on `user/build/` and is not reliable; run them serially for deterministic results.
