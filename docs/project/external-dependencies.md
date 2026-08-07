# 外部依赖集成事实

本文档记录导入组件或外部构建组件的当前集成契约。它描述当前行为，而不是未来计划。

## lwIP

- `EXTERNAL_LWIP_SOURCE_MANIFEST`：内核 lwIP 源文件清单位于 `user/external/lwip/sources.mk`；导入新的 lwIP 代码树时只更新这一份清单。
- `EXTERNAL_LWIP_CONFIG_CONTRACT`：A20OS 通过 `kernel/net/lwip_stack.c` 以 `NO_SYS=1` 模式使用 lwIP，而不是使用 lwIP 自带的 socket API。
- 内核 socket 层拥有 socket 文件，并在 `g_lwip_lock` 保护下转换为 lwIP TCP/UDP/RAW 原语。
- 进展推进基于轮询：`sys_check_timeouts()`、virtio-net TX 完成、RX 投递和 `netif_poll()` 都通过 `a20_lwip_poll()` 与 `kernel_progress_poll()` 驱动。

## QEMU 网络默认值

- `EXTERNAL_QEMU_NET_DEFAULTS`：**已解决。** `10.0.2.15`、`10.0.2.2` 和 `10.0.2.3` 不再编译进内核或用户命令。
- 网络配置只来自命令行或运行时配置。内核命令行接受：
  - `a20.ip=<IPv4>`
  - `a20.netmask=<IPv4>`
  - `a20.gateway=<IPv4>`
  - `a20.dns=<IPv4>`（可重复）
  - `a20.dhcp=1`（覆盖静态 IP/gateway/netmask）
  - `a20.hostname=<name>`
- 如果没有提供这些键且未启用 DHCP，接口会在没有 IPv4 地址的状态下启动；需要路由的 socket 调用返回 `-ENETUNREACH`。
- 生效配置通过只读 `/proc/net/config` 暴露，格式为 `ip=`、`netmask=`、`gateway=`、`dns0=`、`dhcp=` 和 `hostname=` 行。
- QEMU 启动脚本通过追加 `a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os` 保持既有行为。

## 用户态导入项

- `EXTERNAL_USERLAND_UPGRADE_CHECKLIST`：修改 musl、sbase 或 mksh 源码/构建规则后，接受升级前必须运行 Linux 门禁组（`syscall smoke, shell smoke, and coreutils smoke`）。
- `EXTERNAL_STATIC_LINK_REBUILD_CONTRACT`：musl、ABI wrapper、启动代码或 syscall 布局变化要求重建所有静态链接的用户程序；`user/build/<arch>[-nommu]/.build-id` 和 Makefile 源文件时间戳检查会发现陈旧二进制。各架构/MMU 变体使用独立目录，可并行构建而不会互相清理或混入错误 ABI 的程序。

## TLSe 与 wget

- `EXTERNAL_TLSE_WGET_LIMITS`：TLSe/wget 是兼容性工具，不是完整的现代 HTTPS 栈。
- 当前集成不宣称支持 TLS 1.3、现代密码策略或完整证书生态；网络文档和测试不能把 wget 成功当作完整 TLS 支持的证明。
