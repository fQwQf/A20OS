# 网络配置设计

本文档定义 A20OS 的运行时网络配置模型。它替代 `kernel/net/lwip_stack.c` 中硬编码的 QEMU user-network 默认值，以及用户命令中硬编码的 DNS 地址。

## 设计决策

网络配置**只来自命令行或运行时配置**。IP 地址、netmask、gateway、DNS、DHCP 或 hostname 都没有编译期板级默认值。每个开发板、模拟器或部署环境都必须在启动时通过内核命令行提供这些值，或通过下文描述的接口在运行时配置。

做出该决策的原因：

- `10.0.2.15`、`10.0.2.2` 和 `10.0.2.3` 等 QEMU user-network 默认值只是开发便利项，不是架构常量。
- 真实开发板和非 QEMU 后端需要不同地址。
- 运行时配置允许同一内核镜像在多个网络中启动，无需重新构建。
- 这样可以避免在内核中维护每块板子的 `#ifdef` 地址表。

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
- `a20.hostname` 会被复制到 loopback netif，以及该键解析后创建的所有 Ethernet netif。
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

存储位置是 `kernel/net/net_config.c`（新文件）。该结构体初始化为零，然后由命令行解析器填充。`kernel/net/lwip_stack.c` 中现有的 `a20_lwip_register_virtio_netifs()` 改为读取该结构体，不再使用硬编码 QEMU 地址。

## 回退行为

回退行为必须明确且安全：

- 如果没有任何网络键且没有提供 `a20.dhcp`，内核将接口视为未配置。netif 仍会注册并在链路层启动，但没有 IPv4 地址、gateway 或 DNS server。
- 需要路由的网络系统调用在提供有效配置前返回 `-ENETUNREACH`。
- 如果提供了 `a20.dhcp=1`，但 DHCP 交换在用户态启动前尚未完成，接口会保持未配置直到交换完成。用户态必须容忍临时 `-ENETUNREACH` 错误，或等待 config-ready 信号。
- 如果提供 `a20.dhcp=0` 但只提供了部分静态键，缺失值保持为零。例如只有 `a20.ip` 而没有 `a20.netmask` 时，netmask 保持 `0.0.0.0`。

不存在隐藏的 QEMU 地址回退。之前硬编码的 `10.0.2.15`/`10.0.2.2`/`10.0.2.3` 默认值已移除。

## 运行时配置接口

除命令行外，内核通过 `/proc/net/config` 暴露运行时配置：

```text
ip=10.0.2.15netmask=255.255.255.0gateway=10.0.2.2dns0=10.0.2.3dhcp=1hostname=a20os
```

该文件只读，并反映当前生效配置。如果启用 DHCP，租约变化时这些值会更新。

未来可以用 `sys_net_get_config` 和 `sys_net_set_config` 这一对 syscall 替代 proc 文件，以支持原子更新。初始实现使用 proc 文件，避免在锁契约稳定前新增 syscall。

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

用户态读取 `/proc/net/config` 时不持有任何内核 spinlock。proc read 路径会在一个很短的临界区内把配置值复制到临时缓冲区。如果 DHCP 并发更新配置，读者可能看到稍旧的值；这对命令行工具是可接受的。

## 迁移检查清单

实现该设计时，逐项确认：

- [ ] `kernel/net/lwip_stack.c` 不再包含硬编码 QEMU 地址。
- [ ] `a20_lwip_register_virtio_netifs()` 从 `a20_net_config` 读取配置。
- [ ] 命令行解析器识别全部六个 `a20.*` 键。
- [ ] `a20.dhcp=1` 触发 DHCP 并覆盖静态值。
- [ ] `/proc/net/config` 暴露生效配置。
- [ ] `user/cmds/wget.c` 从 `/proc/net/config` 读取 DNS。
- [ ] `user/cmds/ping.c` 从 `/proc/net/config` 读取 DNS。
- [ ] `user/cmds/udpsend.c` 能容忍未配置状态下的 `-ENETUNREACH`。
- [ ] QEMU 启动脚本传入 `a20.ip=10.0.2.15 a20.netmask=255.255.255.0 a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os`，以保持现有 smoke 行为。
- [ ] 网络 smoke 测试覆盖已配置和未配置两种启动路径。
