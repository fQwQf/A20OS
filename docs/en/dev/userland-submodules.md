# User-space Submodules

To be executed after the first checkout or when switching versions:

```sh
git submodule update --init --recursive
```

gitlink ensures that the commit being used is the latest and complete one; do not use `git submodule update --remote` to retrieve the versions.

| Component | Current Source | Fixed Commit |
| --- | --- | --- |
| musl | [upstream](https://git.musl-libc.org/git/musl) | `5122f9f3c99fee366167c5de98b31546312921ab` |
| fastfetch | [A20 fork](https://github.com/fQwQf/fastfetch) | `b349468a9af7b5721bace373c35637f8808827a7` |
| mlibc | [A20 fork](https://github.com/fQwQf/mlibc) | `3a5f8bbcef3c9906dde934d5862e8eed8cb854f5` |
| rtw89 (optional wireless) | [AirPort_RTW89](https://github.com/FranciumOS-org/AirPort_RTW89) | `90b6cfc5a69c271688fad188857cfaeb58053dc5` |

BusyBox 1.37.0 is built from upstream source archives with a fixed SHA-256; standard MMU lean RAMFS uses fixed-version Alpine static packages. For sources and configuration, see [busybox-ramfs.md](busybox-ramfs.md). Production userland has removed the mksh, sbase, and TLSe submodules; shell and common utilities are unified under BusyBox, while musl and fastfetch are retained. MD5 required for PPPoE CHAP is in `user/crypto` and no longer links to sbase libutil.

Only during explicit Native ABI regressions such as `mlibc-mksh` / `mlibc-sbase` does `tools/packaging/fetch_test_source.py` fetch test source code at fixed commits into `build/cache/test-sources`. mksh configuration headers are preserved in `tools/test-fixtures/mksh`. Normal builds and `git submodule update` do not fetch these test samples.

## Integration Guidelines

- **musl**: Uses unmodified official source code. The kernel accepts rtgenmsg dumps, returns RTM_NEWLINK/NEWADDR/NEWROUTE, and generates accurate message lengths; socket ioctls are consistent with rtnetlink for Ethernet/loopback.
- **RV32**: Utilizes Linux's time64 interface and original syscall header files. The kernel correctly handles pselect6's native-word signal descriptors, sigevent/itimer/tms layouts, time64 socket timeouts, _llseek, and mmap2/split-offset parameters. The underlying VFS still uses native long types; file positions outside the RV32 range fail cleanly and are not truncated to 32 bits.
- **musl**: Its own sched_* interfaces still return ENOSYS as defined upstream. A20's Linux scheduling tools and tests directly invoke Linux syscalls via user/cmds/linux_sched.h without overriding libc symbols.
- **mlibc**: Retains the Native ABI's sysdeps/a20, threads, and loader implementation. `.github/workflows` preserves fixed upstream configurations; build files such as ci/a20*.cross-file are retained.
- **rtw89**: The upstream tree is unmodified. A20 platform adapters reside in `user/wifi`, and build scripts prune PCI driver tables in the generated directory. Standalone `rtw89d` links statically against musl with the GPL-2.0 wireless stack; the kernel provides only PCI/DMA and frame transport. See [Wireless Porting](../drivers/classes/rtl8852be-wifi.md).

## Historical Versions

Historical dependency repositories used versions from a20/v0.1 to a20/v0.18 with fixed source code for each version; versions with no code changes shared the same commit. Historical tags continue to use the original snapshots that require patches. a20/current retains the build source code from before the migration; mlibc's CI processes use the unified upstream configuration. The current kernel/user versions do not overwrite the old version snapshots.

The historical migration only ensures the version tag and the build source code from the HEAD state before the migration. For regular commits within the migration period, a cumulative version snapshot is used; it is not guaranteed that each commit can be restored individually. Any historical external dependencies, toolchains, and existing kernel issues will remain from the original version.

## Verification

```sh
make check-upgrade-userland-smokes
python3 -m unittest discover -s tools/tests -p test_riscv32_linux_abi.py -v
make -C user ARCH=riscv32 stock-musl-abi
```

user/tests/test_stock_musl_abi.c verifies interface enumerations of the original musl library, scheduling stubs, direct syscalls, time64 types, signal descriptors, timers, socket timeouts, and file offsets. The runtime must be configured with 10.0.2.15 for the test to proceed; the test program outputs STOCK_MUSL: PASS.

Guest verification on RISC-V64, AArch64, and RISC-V32 covers this probe, poll_edge, timer_edge, and network_suite. Upgrade gates still cover Linux ABI and mlibc Native shell/coreutils. Tests only cover interfaces actually executed at runtime and do not claim full Linux ABI support.

DMA cache helper exports in the RISC-V64 generic driver have been completed, allowing standalone checkouts to load block drivers. RV32 Linux kernel and user programs can be verified independently; full dev-build is currently blocked by existing Native UFSD __atomic_load_8/__atomic_compare_exchange_8 link issues, which are not counted as passed.
