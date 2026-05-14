# A20OS Testing Entry Points

The project should keep a small set of stable commands that are cheap enough
to run during refactors and explicit enough to document what was verified.

## Current Checks

Kernel bringup builds for all supported architectures:

```sh
make check-kernel-build
```

Individual bringup builds:

```sh
make check-riscv64-bringup
make check-loongarch64-bringup
make check-aarch64-bringup
```

Userland builds for all supported architectures:

```sh
make check-user-build
```

Development and contest build entry points:

```sh
make check-dev-build
make check-contest-build
```

## Refactor Rule

For low-level refactors, run at least:

```sh
make ARCH=riscv64 BRINGUP=1 kernel-only -j2
```

For changes touching arch-independent build rules, run:

```sh
make check-kernel-build
```

For syscall, VFS, proc, or user ABI changes, add a userland or QEMU smoke test
before relying on the change long term.

## Smoke Tests

Implemented:

- `make smoke-riscv64`: boot QEMU far enough to confirm init starts.
- `make smoke-loongarch64`: LoongArch64 bringup QEMU boot smoke.
- `make smoke-aarch64`: AArch64 bringup QEMU boot smoke.
- `make smoke-abi-linux`: boot the RISC-V dev image, run `/bin/syscall_smoke`
  through the shell, and require `SYSCALL_SMOKE: PASS` in the log.
- `make smoke-proc-a20`: boot the RISC-V dev image, read
  `/proc/a20/bcache` and `/proc/a20/page_cache`, and require expected cache
  counters in the log.

Planned:

- `make smoke-fs`: create/read/write/rename/unlink/stat on FAT32 and EXT4.
- `make smoke-proc`: fork/exec/wait/signal/futex.
- `make smoke-net`: basic UDP/TCP loopback or QEMU user-net traffic.
- `make smoke-refcount`: debug build covering file/vnode/mm/socket lifetime.
