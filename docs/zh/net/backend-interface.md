# 网络后端接口契约

本文定义 `kernel/include/net/backend.h` 的接口。它是**唯一**允许协议栈实现与 A20OS 其他部分接触的边界；`kernel/net/core/stack.c` 与`kernel/net/core/transport.c` 是唯一的调用方。

设计目标：

1. **模块化**：后端不是一个 30 项的大 vtable，而是 7 个可独立实现/缺失的模块。
2. **能力可查询**：`caps` 是 Linux ABI 可观察行为的承诺，前端据此正确回答`setsockopt`/`getsockopt`/`ioctl`，而不是让未实现的操作静默成功。
3. **低耦合**：前端只持有 fd、队列、等待者和 ABI；后端只持有协议状态。
4. **无静默回退**：实例选择的 `net.stack` 未编译进来时，网络域不可用并明确报错。

---

## 1. 模块总览

```c
typedef struct net_backend {
    const char *name;
    uint32_t caps;
    const net_backend_cycle_ops_t  *cycle;   /* 必备 */
    const net_backend_event_ops_t  *event;   /* 必备 */
    const net_socket_ops_t         *socket;  /* 必备 */
    const net_socket_opt_ops_t     *opts;    /* 可选 */
    const net_endpoint_ops_t       *endpoint;/* 可选 */
    const net_backend_if_ops_t     *iface;   /* 可选 */
    const net_backend_packet_ops_t *packet;  /* 可选 */
    const net_backend_obs_ops_t    *obs;     /* 可选 */
} net_backend_t;
```

| 模块 | 职责 | 缺失时 |
|---|---|---|
| `cycle` | `init` / `attach` / `shutdown` | 后端不可用（注册时拒绝） |
| `event` | 收包推进、IRQ、协议事件、timer、work_pending | 无进展（注册时拒绝） |
| `socket` | open/close/bind/connect/listen/send/shutdown | INET socket 创建失败 |
| `opts` | 缓冲区、IP 选项、协议选项校验 | 使用前端默认值 |
| `endpoint` | `SO_BINDTODEVICE`、每报文控制、PMTU、raw、ioctl | 返回 `-EOPNOTSUPP` |
| `iface` | 接口、地址、路由、邻居事务 | 返回 `-EOPNOTSUPP` |
| `packet` | L2 发送、TUN | AF_PACKET/TUN 返回 `-EOPNOTSUPP` |
| `obs` | `/proc` 只读渲染 | 输出为空 |

**规则**：可选模块可以为 `NULL`；但 `caps` 中置位的每个能力必须有对应模块。两者不一致属于接口缺陷，review 时按 bug 处理。`net_backend_register()` 在启动时执行这项校验：缺少 `open/close/bind/connect/send`、`cycle`、`event`，或`STREAM/SNDBUF/RCVBUF` 没有 `opts`，都会拒绝注册并在串口打印后端名与错误码。

---

## 2. 生命周期

```
net_init()
  └─ net_backend_registry_init()       // 注册本构建编译进来的后端
  └─ net_backend_select(NULL)          // 读取 CONFIG_NET_BACKEND_NAME（来自实例 TOML）
      ├─ 未找到 -> 打印错误，网络域保持不可用；不换栈
      └─ cycle->init()                 // 只做域级初始化，不挂 NIC
...
driver_manager_init()
  └─ net_stack_attach_interfaces()
      └─ cycle->attach()               // 幂等；可以挂 0 个 NIC
```

`cycle->shutdown()` 保留给显式的网络域销毁，当前没有调用者；不得把它当作普通 socket 关闭路径。

---

## 3. socket 模块

```c
typedef struct net_socket_ops {
    int  (*open)(struct net_socket *);
    void (*close)(struct net_socket *);
    void (*release)(struct net_socket *);
    int  (*bind)(struct net_socket *, const void *addr, size_t len);
    int  (*connect)(struct net_socket *, const void *addr, size_t len,
                    const void *peer, size_t peer_len);
    int  (*listen)(struct net_socket *, int backlog);
    int  (*send)(struct net_socket *, const void *buf, size_t len, int flags,
                 const void *addr, size_t addr_len);
    int  (*shutdown)(struct net_socket *, int receive, int transmit);
    void (*accept_ready)(struct net_socket *);
    void (*recved)(struct net_socket *, size_t len);
    void (*close_pcb)(struct net_socket *);
    void (*drop_pcb)(struct net_socket *);
    int  (*has_transport)(const struct net_socket *);
    int  (*has_stream)(const struct net_socket *);
    unsigned (*flags)(const struct net_socket *);
} net_socket_ops_t;
```

语义：

- `open` 在 socket 进入注册表之前调用；失败必须自行释放已经获得的后端资源。
- `close` 释放协议端点；`release` 释放 attachment 本身。二者都可能被调用，必须幂等。
- `bind`/`connect`/`send` 的地址是按 `len` 借用的值拷贝，后端不得保存指针。
- `recved` 只对“由应用消费驱动接收窗口”的栈有意义；BSD 等自动窗口栈可以留`NULL`。前端不会因此伪造窗口更新。
- `accept_ready` 只发布“子 socket 已就绪”，实际入队由后端调用`net_accept_queue_push_locked()` 完成。
- `flags` 返回 poll 层使用的后端就绪位；不参与锁决策。

---

## 4. 选项模块

```c
typedef struct net_socket_opt_ops {
    void (*apply)(struct net_socket *, unsigned mask);
    int  (*validate)(struct net_socket *, int level, int option,
                     const void *value, size_t len);
    uint32_t (*buf_default)(int is_snd);
    uint32_t (*buf_ceiling)(struct net_socket *, int is_snd);
    uint32_t (*buf_in_force)(struct net_socket *, int is_snd);
    void (*ip_apply)(struct net_socket *);
    void (*ip_effective)(struct net_socket *, uint8_t *ttl, uint8_t *tos,
                         uint8_t *mc_ttl);
} net_socket_opt_ops_t;
```

前端负责通用 Linux 解析、权限和默认值；`opts` 只承接协议相关部分。`buf_default` 返回 0 表示“没有协议缓冲区概念”，前端使用自身的静态档位。

---

## 5. 能力位

| 能力 | 含义 | 启用后必须存在 |
|---|---|---|
| `NET_CAP_STREAM` | TCP 类流套接字 | `socket->listen`（若要 listen） |
| `NET_CAP_DGRAM` | UDP | `socket->send` |
| `NET_CAP_RAW` | raw IP | `endpoint->raw_send` |
| `NET_CAP_IPV6` | AF_INET6 收发 | 协议内 IPv6 路径（地址配置见 `IF_ADDR6`） |
| `NET_CAP_LISTEN` | listen/accept | `socket->listen` + accept 队列 |
| `NET_CAP_SHUTDOWN` | 半关闭 | `socket->shutdown` |
| `NET_CAP_SNDBUF/RCVBUF` | 缓冲区选项有协议语义 | `opts->buf_*` |
| `NET_CAP_PKTINFO` | `IP_PKTINFO` / `IPV6_RECVPKTINFO` | 接收元数据填充 |
| `NET_CAP_BINDTODEVICE` | `SO_BINDTODEVICE` | `endpoint->bind_device` |
| `NET_CAP_UDP_CONTROL` | 每报文 ifindex/源地址 | `endpoint->udp_control` |
| `NET_CAP_PMTU` | `IP_MTU_DISCOVER` | `endpoint->pmtu` |
| `NET_CAP_RAW_TX` | 原始 L3 发送 | `endpoint->raw_send` |
| `NET_CAP_TUN` | TUN | `packet->tun_*` |
| `NET_CAP_NEIGH` | 邻居注入 | `iface->neighbor_update_v4` |
| `NET_CAP_ROUTE` | 后端自有 RIB | `iface->route_update_v4` |
| `NET_CAP_IF_ADDR6` | 动态 IPv6 地址 | `iface->set_addr6` |
| `NET_CAP_IF_DYNAMIC` | 运行时 MTU/flags/地址 | 对应 `iface->set_*` |
| `NET_CAP_CONGESTION` | `TCP_CONGESTION` | `opts->validate` + 协议实现 |
| `NET_CAP_DEVICE_IRQ` | 后端消费设备 IRQ | `event->irq` |
| `NET_CAP_MULTI_QUEUE` | 多队列 | 后端自身队列实现 |
| `NET_CAP_OFFLOAD_SG` | 分散聚集发送 | `packet->tx` 的 SG 路径 |
| `NET_CAP_OFFLOAD_CSUM` | 校验和卸载 | 驱动 + 后端双方 |

当前后端：

| 能力 | lwip | native | bsd |
|---|---|---|---|
| stream/dgram | ✓ | ✓ | ✓ |
| raw | ✓ | ✓ | ✓ |
| ipv6 | ✓ | ✓ | 本地 |
| listen/shutdown | ✓ | ✓ | ✓ |
| sndbuf/rcvbuf | ✓ | ✓ | ✓ |
| pktinfo | ✓ | ✓ | ✓ |
| bindtodevice | ✓ | ✓ | ✓（主接口） |
| udp control | ✓ | ✓ | ✓ |
| pmtu | ✓ | ✓ | ✓ |
| raw tx | ✓ | ✓ | ✓ |
| tun | ✓ | ✓ | ✓ |
| neigh | | ✓ | |
| route | | | ✓ |
| ifaddr6 | ✓ | ✓ | ✓ |
| ifdynamic | ✓ | ✓ | ✓ |
| congestion | ✓ (reno) | ✓ | ✓ (newreno/cubic) |
| device irq | ✓ | ✓ | ✓ |
| TX SG | ✓ | ✓ | |

任何一行变成“✓ 但没有模块”都是缺陷。

---

## 6. 前端职责与禁止事项

前端（`kernel/net/socket/`、`kernel/net/core/`）负责：

- fd/vfile 身份、引用计数、权限、等待队列、`epoll` 就绪；
- Linux 通用选项解析、`SCM_RIGHTS`、`AF_UNIX/AF_ALG/AF_NETLINK/AF_PACKET`；
- 接收队列 `net_msg_t` 与 bottom-half 调度；
- 把后端能力翻译成正确的 `ENOPROTOOPT`/`EOPNOTSUPP`，不伪造成功。

前端**不得**：

- 读取后端的 PCB/mbuf/pbuf/ifnet 或 `transport_private` 的内部布局；
- 维护第二份 TCP 状态机或第二套邻居/重组表；
- 在持有后端核心锁时获取 socket 锁；
- 用全局队列复制一份后端已经拥有的 payload。

已知待收敛项：`net_socket_t` 仍包含少量策略缓存字段（`tcp_nodelay` 等）。它们不参与协议状态机，后续应迁入 `opts` 后端 attachment；在迁移完成前不得新增同类字段。

---

## 7. 新增后端清单

1. 在 `kernel/net/backends/<name>/` 实现 `net_backend_t`，至少包含`cycle`、`event`、`socket` 三个模块。
2. 导出 `void a20_<name>_backend_register(void)`，内部调用`net_backend_register(&a20_<name>_backend)`。
3. 在 `kernel/net/sources.mk` 显式列出源文件；在 `Makefile` 增加`CONFIG_NET_BACKEND_<NAME>` 和合法的 `NET_STACK` 值。
4. 在 `kernel/include/net/backend.h` 的能力表中声明真实支持项。
5. 用实例 TOML 选择：`[net] stack = "<name>"`。
6. 跑同一 Linux ELF 的 socket 语义测试、`/proc/net/status` 后端名与能力行、以及至少一条 TCP/UDP 真实收发的验收。

---

## 8. 从旧接口迁移

| 旧字段 | 新位置 |
|---|---|
| `socket_init` | `socket->open` |
| `socket_destroy` | `socket->close` |
| `state_release` | `socket->release` |
| `bind_pcb` | `socket->bind` |
| `tcp_listen` | `socket->listen` |
| `sendto` | `socket->send` |
| `has_tcp` | `socket->has_stream` |
| `transport_flags` | `socket->flags` |
| `options_apply` | `opts->apply` |
| `option_validate` | `opts->validate` |
| `buffer_*` | `opts->buf_*` |
| `ip_options_apply` | `opts->ip_apply` |
| `ip_effective` | `opts->ip_effective` |
| `bind_device` | `endpoint->bind_device` |
| `send_udp_control` | `endpoint->udp_control` |
| `pmtu_discover` | `endpoint->pmtu` |
| `send_raw_packet` | `endpoint->raw_send` |
| `interface_ioctl` | `endpoint->ioctl` |
| `if_*` | `iface->*` |
| `packet_tx` / `tun_*` | `packet->*` |
| `status` / `format_*` | `obs->*` |
| `init` / `attach_interfaces` | `cycle->*` |
| `receive_progress` / `device_interrupt` / … | `event->*` |

旧头文件 `net/transport.h` 现在只 include `net/backend.h`，新代码不得再使用`net_transport_ops_t` 或 `net_stack_ops_t`。

---

## 9. 可观察性

`/proc/net/status` 现在包含：

```
backend: lwip
lwip: stream,dgram,raw,ipv6,listen,shutdown,...
...
```

`backend:` 行来自实际选中的后端，能力行来自 `caps`。两者不一致时，先查`net_backend_t`，再查实例的 `[net].stack`。

## 10. 验证
```sh
# 接口/注册表本身的宿主测试（不依赖 lwIP/FreeBSD）
tools/tests/net_backend_host.sh

# 实例选择与构建目录
make -s NET_INSTANCE=xfce-x86_64 print-build-dir
make -s NET_INSTANCE=release-riscv64 print-build-dir

# 三个后端的编译门禁
make NET_INSTANCE=release-riscv64 ABI=linux BRINGUP=1 kernel-only   # lwip
make ARCH=riscv64 ... NET_STACK=native kernel-only                  # native
make NET_INSTANCE=xfce-x86_64 ABI=linux BRINGUP=1 kernel-only       # bsd

python3 tools/check-network-backend-boundary.py
python3 tools/check-network-backend-boundary.py --check-upstream
```


## 11. 数据面快路径

本次在 lwIP/native 后端落地了三条逐包优化，所有优化都以“没有策略工作”为前提，不改变有规则时的语义：

1. **RX 直接写入 pbuf**：单 lane 路径先取一个缓存的 `PBUF_POOL` 元素，让驱动直接写入 `p->payload`，再 `pbuf_realloc` 到实际长度。旧的`rx_frame -> pbuf_take()` 拷贝消失。空轮询保留 spare，不产生分配。
2. **TX 零拷贝**：无过滤/NAT/conntrack 状态、pbuf 单段且校验和已完整时，直接把`p->payload` 作为借用 iovec 交给 `send_sg`；需要 NAT 或补校验和时才走`tx_frame` 暂存。UDP 校验和 0 按“无校验和”处理。
3. **netfilter 懒检查**：`netfilter_busy()` 是一次原子读。没有过滤规则、NAT 规则、存活 conntrack 表项且未 `cton` 时，RX/TX 跳过 Rust 解析和 conntrack 事务。 conntrack 也因此不再为普通无规则流量建表；需要跟踪时写 `cton`。
4. **BSD 就绪集合**：后端执行器只维护一个 kqueue，socket 创建/accept 时注册`EVFILT_READ`，关闭前删除。每轮进度只对真正就绪的 fd 调用 `recv()`，空闲成本从“每个 socket 一次系统调用”降为“一次 `kevent()`”。有连接在途时才做 connect 完成扫描；kqueue 注册失败时自动回退到旧的完整扫描，不会静默丢就绪。固定门槛见 `user/cmds/net/bsd_scale_test.c`：512 个空闲 UDP socket 加单 socket 32 包突发，全扫描为 16994 次 `recv` 探测，就绪集合为 64 次（上限 1024）。

仍然存在的成本（有意保留或待后续）：

- 多 lane 的 RX 仍先落在 `rx_frame` 再复制进 lane 队列，因为队列所有权和唤醒协议尚未改为 pbuf 引用；
- 协议回调到 socket 队列仍有至少一次拷贝（inline 或 spill），用户态读取再拷贝；
- BSD 后端仍是单执行器，socket 数据的最终发布仍有一次队列拷贝，用户态读取再拷贝一次；kqueue 只消除了空闲 socket 的逐轮 `recv()`，RX/TX 尚未零拷贝。其 RX/TX 经过 `netfilter_bridge.c` 接入同一套过滤/NAT/conntrack，而不是绕过策略层；
- 没有规则时的 netfilter 快路径不统计 conntrack 包，这是懒启用的直接结果。

这些优化没有改变任何 ABI；`/proc/net/status` 的 backend/caps 行和`/proc/a20/netfilter` 的规则语义保持不变。
