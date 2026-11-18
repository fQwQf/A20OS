# 用户态 submodule

首次检出或切换版本后执行：

```sh
git submodule update --init --recursive
```

gitlink 固定完整 commit；不要使用 `git submodule update --remote` 替代版本检出。

| 组件 | 当前来源 | 固定 commit |
| --- | --- | --- |
| musl | [upstream](https://git.musl-libc.org/git/musl) | `5122f9f3c99fee366167c5de98b31546312921ab` |
| fastfetch | [A20 fork](https://github.com/fQwQf/fastfetch) | `b349468a9af7b5721bace373c35637f8808827a7` |
| mlibc | [A20 fork](https://github.com/fQwQf/mlibc) | `3a5f8bbcef3c9906dde934d5862e8eed8cb854f5` |
| rtw89（可选无线服务） | [AirPort_RTW89](https://github.com/FranciumOS-org/AirPort_RTW89) | `90b6cfc5a69c271688fad188857cfaeb58053dc5` |

BusyBox 1.37.0 由固定 SHA-256 的上游源码归档构建；普通 MMU 精简 RAMFS 使用固定版本的 Alpine 静态包。来源和配置见 [busybox-ramfs.md](busybox-ramfs.md)。生产用户态已移除 mksh、sbase 和 TLSe 的 submodule，shell 和通用命令统一由 BusyBox 提供；musl 与 fastfetch 保留。PPPoE CHAP 所需的公共领域 MD5 位于`user/crypto`，不再链接 sbase libutil。

仅显式运行 `mlibc-mksh` / `mlibc-sbase` 等 Native ABI 回归时，`tools/packaging/fetch_test_source.py` 才获取固定提交的测试源码，放在`build/cache/test-sources`。mksh 配置头在 `tools/test-fixtures/mksh`。普通构建和 `git submodule update` 不获取这两个测试样本。

## 集成约定

- musl 为未经修改的官方源码。内核接受 rtgenmsg dump，返回 RTM_NEWLINK/NEWADDR/NEWROUTE，并生成准确的消息长度；socket ioctl 与 rtnetlink 一致标记 Ethernet/loopback。
- RV32 使用 Linux 的 time64 入口及原版 syscall 头文件。内核正确处理 pselect6 的 native-word 信号描述符、sigevent/itimer/tms 布局、time64 socket timeout、_llseek 和 mmap2/split-offset 参数。底层 VFS 仍使用 native long；超出 RV32 表示范围的文件位置明确失败，不再截断到低 32 位。
- musl 自身的四个 sched_* 接口仍返回上游定义的 ENOSYS。A20 自有 Linux 调度工具和测试通过 user/cmds/linux_sched.h 直接调用 Linux syscall；不覆盖 libc 符号。
- mlibc 保留 Native ABI 的 sysdeps/a20、线程和 loader 移植。`.github/workflows` 保持固定上游配置；ci/a20*.cross-file 等构建文件保留。
- rtw89 上游树不修改，A20 平台适配位于 `user/wifi`，构建脚本在生成目录裁剪 PCI 驱动表。独立 `rtw89d` 静态链接 musl，包含 GPL-2.0 无线栈；内核只提供 PCI/DMA 和帧传输。详见 [无线移植](../drivers/classes/rtl8852be-wifi.md)。

## 历史版本

历史依赖仓库用 a20/v0.1～a20/v0.18 固定各版本源码；没有源码变化的版本共用 commit。历史 tag 继续使用需要补丁的原始 snapshot。a20/current 保留迁移前 HEAD 的构建源码；mlibc CI 统一使用上游配置。当前版本的 kernel/user 修复不回写旧版本快照。

历史迁移只保证版本 tag 和迁移前 HEAD 的构建源码；区间内普通提交使用累计版本 snapshot，不保证逐提交还原。历史外部依赖、工具链及既有内核问题仍属于原版本。

## 验证

```sh
make check-upgrade-userland-smokes
python3 -m unittest discover -s tools/tests -p test_riscv32_linux_abi.py -v
make -C user ARCH=riscv32 stock-musl-abi
```

user/tests/test_stock_musl_abi.c 验证原版 musl 的接口枚举、调度 stub 与直接 syscall、time64、信号描述符、timer、socket timeout 及文件偏移。运行时需配置 10.0.2.15，测试程序输出 STOCK_MUSL: PASS。

RISC-V64、AArch64、RISC-V32 的 guest 验证包括该探针、poll_edge、timer_edge 和 network_suite。升级门禁仍覆盖 Linux ABI 与 mlibc Native shell/coreutils。测试只覆盖这些实际运行的接口，不宣称完整 Linux ABI 支持。

RISC-V64 generic driver 的 DMA cache helper 导出已补齐，使独立检出可以加载块驱动。RV32 Linux 内核和用户程序可独立验证；完整 dev-build 目前仍被既有 Native UFSD 的 __atomic_load_8/__atomic_compare_exchange_8 链接问题阻挡，该失败没有计为通过。
