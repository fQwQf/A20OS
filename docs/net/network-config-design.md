# 网络配置设计

本文档记录 A20OS 当前的网络配置模型。`kernel/net/lwip_stack.c` 不再直接硬编码地址，但部分板级 `arch_bootargs_get()` 仍返回编译进内核的 `a20.*` 参数；这与由启动器传入 bootargs 是两个不同来源。

## 设计决策

网络栈只解析 `bootargs_get()` 返回的 `a20.*` 键，并可由 DHCP 更新生效状态。bootargs 的来源依架构/板级而异：RISC-V64/RISC-V32/PPC64LE 从 FDT `/chosen/bootargs` 读取；QEMU AArch64 和 VirtualBox AArch64 因尚未接入外部命令行，分别在板级源码中返回编译期 NAT 默认参数；没有实现 hook 的平台返回空配置。

做出该决策的原因：

- `10.0.2.15`、`10.0.2.2` 和 `10.0.2.3` 等 QEMU user-network 默认值只是开发便利项，不是架构常量。
- 真实开发板和非 QEMU 后端需要不同地址。
- 运行时配置允许同一内核镜像在多个网络中启动，无需重新构建。
- 长期目标是让同一镜像完全由启动器提供配置；当前两个 AArch64 板级默认是明确记录的过渡例外。

## 当前 bootargs 来源

| 目标 | 来源 | 无外部参数时的行为 |
|---|---|---|
| RISC-V64、RISC-V32、PPC64LE | FDT `/chosen/bootargs` | 没有属性则未配置；部分 QEMU run/smoke 目标用 `-append` 注入 NAT 参数 |
| QEMU AArch64 | `kernel/platform/qemu-virt-aarch64/board.c` 的编译期字符串 | `10.0.2.15/24`、gateway `10.0.2.2`、DNS `10.0.2.3`、hostname `a20os-qemu` |
| VirtualBox AArch64 | `kernel/platform/virtualbox-aarch64/board.c` 的编译期字符串 | 同一 NAT 地址，hostname `a20os-vbox` |
| 其他未实现 `arch_bootargs_get()` 的目标 | weak hook 返回 `NULL` | 未配置 |

QEMU AArch64 当前不导入 FDT `/chosen/bootargs`，所以给 QEMU 增加 `-append` 不能覆盖板级编译字符串。要改变该目标的运行时参数，必须先实现真实的 bootargs handoff；不能把文档中的理想覆盖语义当成当前能力。

## 内核命令行键

内核识别以下命令行键。所有键都是可选的。

| 键 | 值 | 示例 |
|-----|-------|---------|
| `a20.ip` | 点分十进制 IPv4 地址 | `a20.ip=10.0.2.15` |
| `a20.netmask` | 点分十进制 IPv4 netmask | `a20.netmask=255.255.255.0` |
| `a20.gateway` | IPv4 默认网关 | `a20.gateway=10.0.2.2` |
| `a20.dns` | IPv4 DNS 服务器，可出现多次 | `a20.dns=10.0.2.3` |
| `a20.dhcp` | `1` 启用 DHCP，`0` 禁用 | `a20.dhcp=1` |
| `a20.hostname` | hostname 字符串，最长 63 字节 | `a20.hostname=a20os` |

### 键规则

- `a20.dhcp=1` 优先于静态 `a20.ip`、`a20.netmask` 和 `a20.gateway`。内核运行 DHCP，并使用租约填充运行时配置。
- 如果 `a20.dhcp=0` 或没有提供 `a20.dhcp`，内核使用静态值。
- `a20.dns` 可以出现多次。第一次填充 DNS server slot 0，第二次填充 slot 1，依此类推，直到 lwIP DNS server 数量上限。
- `a20.hostname` 当前只赋给 loopback netif；Ethernet netif 初始化尚未设置 `hostname` 字段。
- 所有值都在早期启动期间解析一次，并存储到运行时 `a20_net_config` 结构体中。

## 解析位置

内核在处理其他启动参数的同一轮早期命令行遍历中解析网络键。解析后的值存放在一个结构体中：

```c
typedef struct a20_net_config {
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    ip4_addr_t dns[LWIP_DNS_SERVER_LIST_SIZE];
    int        dns_count;
    int        dhcp_enable;
    char       hostname[64];
} a20_net_config_t;
```

存储位置是 `kernel/net/net_config.c`。该结构体初始化为零，然后从 `bootargs_get()` 的完整字符串解析。`kernel/net/lwip_stack.c` 的 `a20_lwip_register_netifs()` 读取该结构体，不再自行选择 QEMU 地址；板级编译默认仍会以普通 bootargs 的形式进入同一解析器。

## 回退行为

回退行为必须明确且安全：

- 如果最终 bootargs 没有任何网络键且没有提供 `a20.dhcp`，内核将接口视为未配置。netif 仍会注册并在链路层启动，但没有 IPv4 地址、gateway 或 DNS server。QEMU/VirtualBox AArch64 因板级编译默认不满足此前提。
- 需要路由的网络系统调用在提供有效配置前返回 `-ENETUNREACH`。
- 如果提供了 `a20.dhcp=1`，但 DHCP 交换在用户态启动前尚未完成，接口会保持未配置直到交换完成。用户态必须容忍临时 `-ENETUNREACH` 错误，或等待 config-ready 信号。
- 如果提供 `a20.dhcp=0` 但只提供了部分静态键，缺失值保持为零。例如只有 `a20.ip` 而没有 `a20.netmask` 时，netmask 保持 `0.0.0.0`。

lwIP 层不存在额外的隐藏地址回退；但 QEMU AArch64 与 VirtualBox AArch64 的 `arch_bootargs_get()` 明确编译了 `10.0.2.15`/`10.0.2.2`/`10.0.2.3`。其他平台是否得到这些值取决于启动器是否传入对应 bootargs。

## 运行时配置接口

除命令行外，内核通过 `/proc/net/config` 暴露运行时配置：

```text
ip=10.0.2.15
netmask=255.255.255.0
gateway=10.0.2.2
dns0=10.0.2.3
dhcp=1
hostname=a20os
```

该文件只读，并反映当前生效配置。如果启用 DHCP，租约变化时这些值会更新。

**未来计划**：可以增加 `sys_net_get_config`/`sys_net_set_config` 来支持原子更新。当前没有这些 syscall，`/proc/net/config` 只读，不能作为运行时设置入口。

## 用户命令消费方式

以下用户命令消费运行时配置。

### `wget`

- 移除硬编码的 `DNS_SERVER_IP` 常量。
- 从 `/proc/net/config` 读取第一个 DNS server。
- 如果没有配置 DNS server，`wget` 在发送任何查询前打印错误并退出。
- 其他行为保持不变，包括 URL 解析、TCP 连接、TLS 握手和 HTTP 获取。

### `ping`

- 移除硬编码的 `DNS_SERVER_IP` 常量。
- 当参数是 hostname 时，从 `/proc/net/config` 读取第一个 DNS server。
- 如果参数已经是 IPv4 字面量，不执行 DNS 查询。
- 如果没有配置 DNS server 且参数是 hostname，`ping` 打印错误并退出。

### `udpsend`

- `udpsend` 通过命令行参数接收目标 IPv4 地址和端口，因此不需要 DNS 查询。
- 它仍依赖接口已配置。如果接口没有地址或路由，内核 `sendto()` 路径返回 `-ENETUNREACH`。
- `udpsend` 未来可以增加可选 `-i` 参数，用于打印 `/proc/net/config` 中的当前网络配置，但默认行为保持不变。

## DHCP 与锁的交互

DHCP 作为 lwIP timeout 处理的一部分运行。它在更新 netif 地址和 DNS server 状态时持有 `g_lwip_lock`。为避免给用户态造成意外，DHCP 产生的地址变化会在持有 `g_lwip_lock` 时同时提交到 lwIP netif 和运行时 `a20_net_config` 结构体。

用户态读取 `/proc/net/config` 时，`a20_net_config_format()` 会短暂获取 `g_lwip_lock`，先把默认 netif 和 DNS 状态同步到 `g_a20_net_config`，释放锁后再格式化文本。读取不会与 DHCP 更新并发撕裂 lwIP 状态，但它不是无锁快照。

## 迁移检查清单

实现该设计时，逐项确认（该设计已实现，`smoke-network-suite` 覆盖）：

- [x] `kernel/net/lwip_stack.c` 不再包含硬编码 QEMU 地址。
- [x] `a20_lwip_register_netifs()` 从 `a20_net_config` 读取配置。
- [x] 命令行解析器识别全部六个 `a20.*` 键。
- [x] `a20.dhcp=1` 触发 DHCP 并覆盖静态值。
- [x] `/proc/net/config` 暴露生效配置。
- [x] `user/cmds/net/wget.c` 从 `/proc/net/config` 读取 DNS。
- [x] `user/cmds/net/ping.c` 从 `/proc/net/config` 读取 DNS。
- [x] `user/cmds/net/udpsend.c` 能容忍未配置状态下的 `-ENETUNREACH`。
- [x] 需要 NAT 配置的部分 QEMU run/smoke 目标传入 `a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os`；两个 AArch64 板级目标使用上述编译期 bootargs。
- [x] 网络 smoke 测试覆盖已配置和未配置两种启动路径。
