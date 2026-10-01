# 外部依赖集成事实

## 源码存储形态

- `kernel/external/lwip` 与 `user/external/{musl,mlibc,mksh-cvs2git,sbase,tlse}` 是普通 tracked tree。
- `.gitmodules` 目前只登记 `user/external/apps/fastfetch` 一项；超级项目的 gitlink 条目固定具体 commit；许可证必须对该精确 commit 核验。
- fastfetch 是唯一仍从源码编译进根文件系统的独立第三方程序：它被链接进 FAT32 根，而该根在任何 Alpine world 挂载之前就要启动。
- 基础 `user/Makefile` 存在静态 musl 链接；分发分析不能把用户程序一概描述为与 musl 分离。

## 已退役的源码自建用户态路径

`user/external/{apps/{git,vim},libs/zlib,toolchain/{binutils,musl-cross-make},gcc,rust,toolchain/Lamina1}` 与 `user/extra.mk` 已删除：这些程序现由 `packages/world/*.world` 清单经 `apk` 从 Alpine 上游仓库解析。`tools/targets-extra.mk` 保留了 `extra-img`、`run-<arch>-extra`、`vf2-extra` 等入口作为**会报错的弃置桩**，每个都给出替代命令；这是刻意的——让旧命令失败时直接告诉正确做法，而不是让它报 "no rule to make target"。

`tools/img.py extra`、`tools/stamps.py extra-inputs` 与 `tools/pkg.py prepare-riscv64-sysroot` 已随之删除。真板仍需要一块可写 ext4（挂 `/extra`），该分区现在由 world 镜像填充：`mkrootfs.py` 输出的无分区表 raw ext4 可直接替换原 `extra.img`，`make vf2-sdcard` 即走此路径（`VF2_WORLD` 选 world，默认 `devel`）。

被删除的 glibc sysroot 引导（`prepare-riscv64-glibc-sysroot.sh`）曾为源码构建的 glibc 动态链接程序提供运行库；Alpine 包自带各自 loader，因此不再需要。

## lwIP

- `EXTERNAL_LWIP_SOURCE_MANIFEST`：内核 lwIP 源文件清单位于 `kernel/external/lwip/sources.mk`；导入新的 lwIP 代码树时只更新这一份清单。
- `EXTERNAL_LWIP_CONFIG_CONTRACT`：A20OS 通过 `kernel/net/lwip_stack.c` 以 `NO_SYS=1` 模式使用 lwIP，而不是使用 lwIP 自带的 socket API。
- 内核 socket 层拥有 socket 文件，并在 `g_lwip_lock` 保护下转换为 lwIP TCP/UDP/RAW 原语。
- 进展推进基于轮询：`sys_check_timeouts()`、virtio-net TX 完成、RX 投递和 `netif_poll()` 都通过 `a20_lwip_poll()` 与 `kernel_progress_poll()` 驱动。

## 网络默认值与 bootargs

- `EXTERNAL_QEMU_NET_DEFAULTS`：lwIP 与用户命令不再自行硬编码 NAT 地址，但 QEMU AArch64 与 VirtualBox AArch64 的板级 `arch_bootargs_get()` 仍把 `10.0.2.15`、`10.0.2.2` 和 `10.0.2.3` 编译为过渡 bootargs；它们当前没有外部命令行 handoff。
- RISC-V64/RISC-V32/PPC64LE 从 FDT `/chosen/bootargs` 读取参数；没有 hook 或没有 bootargs 的其他平台保持未配置。解析器接受：
  - `a20.ip=<IPv4>`
  - `a20.netmask=<IPv4>`
  - `a20.gateway=<IPv4>`
  - `a20.dns=<IPv4>`（可重复）
  - `a20.dhcp=1`（覆盖静态 IP/gateway/netmask）
  - `a20.hostname=<name>`
- 如果没有提供这些键且未启用 DHCP，接口会在没有 IPv4 地址的状态下启动；需要路由的 socket 调用返回 `-ENETUNREACH`。
- 生效配置通过只读 `/proc/net/config` 暴露，格式为 `ip=`、`netmask=`、`gateway=`、`dns0=`、`dhcp=` 和 `hostname=` 行。
- 部分 QEMU run/smoke 目标通过 `-append` 注入 NAT 参数；这只对实际导入 bootargs 的架构生效，不能覆盖当前 QEMU AArch64 的板级编译字符串。

## 用户态导入项

- `EXTERNAL_USERLAND_UPGRADE_CHECKLIST`：修改 musl、sbase 或 mksh 源码/构建规则后，接受升级前必须运行 Linux 门禁组：`make check-upgrade-userland-smokes` 聚合 `smoke-abi-linux`（syscall）、`smoke-mlibc-mksh`（shell）、`smoke-mlibc-sbase`（coreutils）；lwIP 另见网络节，TLSe/wget 见各自集成说明。
- `EXTERNAL_STATIC_LINK_REBUILD_CONTRACT`：musl、ABI wrapper、启动代码或 syscall 布局变化要求重建所有静态链接的用户程序；`user/build/<arch>[-nommu]/.build-id` 和 Makefile 源文件时间戳检查会发现陈旧二进制。各架构/MMU 变体使用独立目录，可并行构建而不会互相清理或混入错误 ABI 的程序。

## TLSe 与 wget

- `EXTERNAL_TLSE_WGET_LIMITS`：TLSe/wget 是兼容性工具，不是完整的现代 HTTPS 栈。
- 仓库跟踪的 `user/external/tlse/LICENSE` 提供 BSD-2-Clause 或 Unlicense 二选一，不是 MIT。
- 当前集成不宣称支持 TLS 1.3、现代密码策略或完整证书生态；网络文档和测试不能把 wget 成功当作完整 TLS 支持的证明。
