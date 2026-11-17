# 外部依赖集成事实

## 源码存储形态

- `kernel/external/uacpi` 是普通 tracked tree；原版 lwIP 与 BSD 候选 F-Stack 使用 submodule；`user/external/{musl,mlibc}` 与 fastfetch 使用固定 commit 的 submodule。
- `.gitmodules` 登记依赖来源，超级项目的 gitlink 固定具体 commit；许可证必须对该精确 commit 核验。首次检出运行 `git submodule update --init --recursive`。来源、保留适配和版本历史约定见 [userland-submodules.md](userland-submodules.md)。
- 非 world 用户态以 BusyBox 提供 shell 和标准工具，保留 fastfetch 与 musl。平台 init 和 A20 专用诊断/网络工具继续独立构建。BusyBox 源码归档和普通 RAMFS 静态包均固定版本及 SHA-256，见 [busybox-ramfs.md](busybox-ramfs.md)。
- 基础 `user/Makefile` 存在静态 musl 链接；分发分析不能把用户程序一概描述为与 musl 分离。

## 已退役的源码自建用户态路径

`user/external/{apps/{git,vim},libs/zlib,toolchain/{binutils,musl-cross-make},gcc,rust,toolchain/Lamina1}` 与 `user/extra.mk` 已删除：这些程序现由 `packages/world/*.world` 清单经 `apk` 从 Alpine 上游仓库解析。`tools/mk/targets-extra.mk` 保留了 `extra-img`、`run-<arch>-extra`、`vf2-extra` 等入口作为**会报错的弃置桩**，每个都给出替代命令；这是刻意的——让旧命令失败时直接告诉正确做法，而不是让它报 "no rule to make target"。

`tools/img.py extra`、`tools/stamps.py extra-inputs` 与 `tools/pkg.py prepare-riscv64-sysroot` 已随之删除。真板仍需要一块可写 ext4（挂 `/extra`），该分区现在由 world 镜像填充：`mkrootfs.py` 输出的无分区表 raw ext4 可直接替换原 `extra.img`，`make vf2-sdcard` 即走此路径（`VF2_WORLD` 选 world，默认 `devel`）。

被删除的 glibc sysroot 引导（`prepare-riscv64-glibc-sysroot.sh`）曾为源码构建的 glibc 动态链接程序提供运行库；Alpine 包自带各自 loader，因此不再需要。

## uACPI

`kernel/external/uacpi` 保存未经修改的上游 7.0.0 `source/` 与 `include/`；固定 revision、归档 SHA256 与许可证入口见其 `A20OS-SOURCE`。ACPI 表、namespace 与 AML 适配位于 `kernel/firmware/acpi/`；物理映射、PCI 配置、端口访问和 SCI 入口在各架构的 `arch/` 实现。更新上游后须运行 `make check-firmware-model` 的生产 namespace fixture，并验证相应平台的 ACPI 启动和 S5 关机。同步时保留上游 MIT `LICENSE`。

## lwIP

- `EXTERNAL_LWIP_SOURCE_MANIFEST`：内核 lwIP 源文件清单位于 `kernel/net/backends/lwip/sources.mk`；导入新的 lwIP 代码树时只更新这一份清单。
- `EXTERNAL_LWIP_CONFIG_CONTRACT`：A20OS 通过 `kernel/net/backends/lwip/lwip_stack.c` 以 `NO_SYS=1` 模式使用 lwIP，而不是使用 lwIP 自带的 socket API。
- 内核 socket 层拥有 socket 文件，并在 `g_lwip_lock` 保护下转换为 lwIP TCP/UDP/RAW 原语。
- 进展推进基于轮询：`sys_check_timeouts()`、virtio-net TX 完成、RX 投递和 `netif_poll()` 都通过 `a20_lwip_poll()` 与 `kernel_progress_poll()` 驱动。
- 原版 `NET_STACK=lwip` 固定于 `kernel/external/lwip-upstream`，构建门禁检查提交与干净状态；该目录不写入适配。
- `EXTERNAL_LWIP_IS_A_FORK`：旧实现已迁入 `kernel/net/backends/native/protocol`，以 `NET_STACK=native` 独立维护，不再作为外部原版 lwIP。版权与历史差异清单保留，当前归属见 [native README](../../../kernel/net/backends/native/README.md)。
- BSD 的 F-Stack submodule 尚不能直接链接到内核；具体依赖与进度见 [bsd-port.md](../net/bsd-port.md)。

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

## 外部 QEMU 源码定位

驱动与网络文档引用的 `qemu-10.0.13+ds/...` 路径**不属于本仓库**：它们指向 Debian 源码包 `qemu` 版本 `1:10.0.13+ds-0+deb13u1` 的解包目录。把 `.dsc` 与两个 tar 包下载到仓库外同一目录，再执行 `dpkg-source -x qemu_10.0.13+ds-0+deb13u1.dsc`；引用中的路径相对于该解包目录，不是 A20OS checkout 内的文件。

以下 SHA-256 用于固定所引用的外部源码版本（未重新下载或核验 Debian 签名）：

| 文件 | SHA-256 |
| --- | --- |
| `qemu_10.0.13+ds.orig.tar.xz` | `6a0888e806c2ffc0c5f0733e974f9aea55491950d3c47c98669c8b236dde59ae` |
| `qemu_10.0.13+ds-0+deb13u1.debian.tar.xz` | `da73c092f5684c551b0334d675eb16a3dec91e1edea94759cfb1102fa4a0364e` |

## 用户态导入项

- `EXTERNAL_USERLAND_UPGRADE_CHECKLIST`：修改 musl 或可选 Native ABI 测试样本的构建规则后，接受升级前必须运行 Linux 门禁组：`make check-upgrade-userland-smokes` 聚合 `smoke-abi-linux`（syscall）、`smoke-mlibc-mksh`（shell）、`smoke-mlibc-sbase`（coreutils）；lwIP 另见网络节。mksh/sbase 仅作为按需下载的测试样本，不是生产依赖。
- `EXTERNAL_STATIC_LINK_REBUILD_CONTRACT`：musl、ABI wrapper、启动代码或 syscall 布局变化要求重建所有静态链接的用户程序；`user/build/<arch>[-nommu]/.build-id` 和 Makefile 源文件时间戳检查会发现陈旧二进制。各架构/MMU 变体使用独立目录，可并行构建而不会互相清理或混入错误 ABI 的程序。

## BusyBox wget 与 PPPoE

TLSe 和自有 wget 已移除；源码构建的 BusyBox wget 开启上游内置 TLS，关闭
独立用户态中不存在的 OpenSSL helper。HTTP 重定向、错误状态和 HTTPS 下载由
`tools/tests/check_upstream_downloads.py` 对本地对端验证。内置 TLS 不校验服务器
证书，下载成功不代表认证成功；需要证书验证时应使用 world 的上游 curl/wget
及 CA 包。普通精简 RAMFS 的 Alpine busybox-static 未启用 HTTPS。

PPPoE 仍是 A20 的 BSD TUN 适配工具；其 CHAP-MD5 直接链接 `user/crypto/md5.c`
中的公共领域实现，保留原认证协议，不再依赖整套 sbase。真实 Linux pppd/pppoe-server
对端回归入口为 `tools/net/tests/test-bsd-pppoe-kvm.py`，需要 `NET_ROLE=router` 内核。
