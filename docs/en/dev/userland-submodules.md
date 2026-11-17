# User-space submodule

To be executed after the first checkout or when switching versions:

```sh
git submodule update --init --recursive
```

gitlink ensures that the commit being used is the latest and complete one; do not use `git submodule update --remote` to retrieve the versions.  

| Component | Current Source | Fixed Commit |
| --- | --- | --- |
| musl | [upstream](https://git.musl-libc.org/git/musl) | `5122f9f3c99fee366167c5de98b31546312921ab` |
| sbase | [upstream](https://git.suckless.org/sbase) | `c1341583c96307cb0e6152c963ed23c4d56a4278` |
| mksh-cvs2git | [upstream](https://github.com/mirabilos/mksh-cvs2git) | `341752823184238dc9b2b0a22b48da984f25442d` |
| mlibc | [A20 fork](https://github.com/fQwQf/mlibc) | `3a5f8bbcef3c9906dde934d5862e8eed8cb854f5` |
| tlse | [upstream](https://github.com/eduardsui/tlse) | `e69790bb14e35e2db6683f6193cb2b685780f7aa` |

## Integration Guidelines

- **musl**: Uses the unmodified official source code. The kernel accepts rtgenmsg dumps, returns RTM_NEWLINK/NEWADDR/NEWROUTE, and generates the correct message lengths; socket ioctl functions are consistent with rtnetlink for Ethernet/loopback support.
- **RV32**: Utilizes Linux’s time64 interface and the original syscall header files. The kernel correctly handles pselect6’s native-word signal descriptors, sigevent/itimer/tms layouts, time64 socket timeouts, _llseek, and mmap2/split-offset parameters. The underlying VFS still uses native long types; file positions outside the RV32 range result in clear failures and are not truncated to the lower 32 bits.
- **musl**: Its own sched_* interfaces still return ENOSYS as defined by the upstream. A20’s native Linux scheduling tools and tests directly call Linux system calls via user/cmds/linux_sched.h without overriding libc symbols.
- **mlibc**: Retains the Native ABI’s sysdeps/a20, as well as thread and loader implementations. The `.github/workflows` files maintain the fixed upstream configuration; build files like ci/a20*.cross-file are also preserved.
- **mksh**: Uses the original source code; configuration and header files are located in user/ports/mksh.
- **sbase**: Uses the original source code; additional id commands are found in user/ports/sbase, and the MIT LICENSE is maintained.
- **TLSe**: Uses the original fixed snapshot, removing the A20 fast-math/cipher patches.

## Historical Versions

Historical dependency repositories used versions from a20/v0.1 to a20/v0.18 with fixed source code for each version; versions with no code changes shared the same commit. Historical tags continue to use the original snapshots that require patches. a20/current retains the build source code from before the migration; mlibc’s CI processes use the unified upstream configuration. The current kernel/user versions do not overwrite the old version snapshots.

The historical migration only ensures the version tag and the build source code from the HEAD state before the migration. For regular commits within the migration period, a cumulative version snapshot is used; it is not guaranteed that each commit can be restored individually. Any historical external dependencies, toolchains, and existing kernel issues will remain from the original version.

## Verification

```sh
make check-upgrade-userland-smokes
python3 -m unittest discover -s tools/tests -p test_riscv32_linux_abi.py -v
make -C user ARCH=riscv32 stock-musl-abi
```

The file user/tests/test_stock_musl_abi.c verifies the interface enumerations of the original musl library, as well as the scheduling stubs, direct system calls, time64 types, signal descriptors, timers, socket timeouts, and file offsets. The runtime must be configured to use version 10.0.2.15 for the test to proceed successfully; the test program should output STOCK_MUSL: PASS.

For guests on RISC-V64, AArch64, and RISC-V32, the verification includes tests for the poll_edge, timer_edge, and network_suite components. The access control system upgrades also cover the Linux ABI and the mlibc Native shell/coreutils. The tests only cover the interfaces that are actually in use during runtime and do not claim to provide full support for the entire Linux ABI.

The DMA cache helper functions in the RISC-V64 generic driver have been updated, allowing for the independent detection and loading of block drivers. The RV32 Linux kernel and user programs can be verified separately. However, the complete dev-build process is currently hindered by issues with the __atomic_load_8 and __atomic_compare_exchange_8 functions in the existing Native UFSD library. These failures are not counted as successful test results.
