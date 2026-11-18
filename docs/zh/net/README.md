# 网络子系统文档索引

A20OS 网络子系统支持双栈（IPv4/IPv6）以及可插拔协议栈后端架构，兼顾低资源占用、高并发扩展性与软路由转发能力。

---

## 1. 核心架构与后端契约

| 文档 | 说明 |
|---|---|
| [architecture.md](architecture.md) | 网络子系统总体架构、数据面分层与生命周期 |
| [backend-interface.md](backend-interface.md) | 协议栈后端接口契约（lwIP / native / BSD 统一接口） |
| [backend-policy.md](backend-policy.md) | 后端选型策略与构建期/运行期切换规则 |
| [instance-config.md](instance-config.md) | 实例网络配置（`instances/*.toml` 中的网络字段解析） |
| [network-config-design.md](network-config-design.md) | 网络参数（`a20.*`）与运行时控制面设计 |

## 2. 协议栈实现与集成

| 文档 | 说明 |
|---|---|
| [lwip-upstream.md](lwip-upstream.md) | 原版 upstream lwIP 2.2.1 submodule 接入与无修改适配契约 |
| [lwip-allocator-locks.md](lwip-allocator-locks.md) | lwIP 内存分配器并发锁机制与隔离 |
| [bsd-port.md](bsd-port.md) | FreeBSD / F-Stack 网络协议栈接入设计与当前移植状态 |

## 3. 软路由、NAT 与网络功能

| 文档 | 说明 |
|---|---|
| [routing.md](routing.md) | 核心路由分发、策略路由与多网卡转发机制 |
| [router.md](router.md) | 软件路由器实现、校验和重算与差错处理 |
| [nat.md](nat.md) | NAT 基础机制与地址端口转换规则 |
| [conntrack-nat.md](conntrack-nat.md) | 连接跟踪（conntrack）与动态 NAPT 状态机 |
| [nat-host-oracle.md](nat-host-oracle.md) | NAT 行为与 Linux 参照对比验证基准 |
| [lan-services.md](lan-services.md) | 局域网服务支撑（ARP、NDP、DHCP 广播接入） |

## 4. 并发、锁契约与性能

| 文档 | 说明 |
|---|---|
| [network-lock-contract.md](network-lock-contract.md) | 网络核心锁契约：锁层次、断言规则与死锁避免 |
| [net-lanes.md](net-lanes.md) | 多 Lane 并发收发调度设计与阶段推进 |
| [checksum-offload.md](checksum-offload.md) | 硬件网卡 Checksum Offload 设计与边界调查 |
| [benchmark-method.md](benchmark-method.md) | 网络性能吞吐/延迟测量基线与方法学 |

## 5. 验证与测试

网络栈的自动化门禁覆盖双后端支持与自动化回归：
- 回归测试与自动化门禁详见 [测试门禁总览](../testing-gates.md)。
- 性能与基准测试方法学参见 [benchmark-method.md](benchmark-method.md)。
