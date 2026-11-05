# 实例 TOML 与网络后端选择

一个构建/运行配置由 `instances/<name>.toml` 描述。网络协议栈、并发度、资源档位和角色都属于实例声明，不再靠手写 `NET_STACK=`/`NET_LANES=` 记忆。

## 1. 字段

```toml
[net]
# 协议栈：lwip | native | bsd | none
stack = "bsd"

# 可选：执行并发度，对应 NET_LANES
lanes = 1

# 可选：资源档位，对应 NET_PROFILE
#   1 = embedded, 2 = default, 3 = server
resource_profile = 2

# 可选：host | router，对应 NET_ROLE
role = "host"

# 可选：BSD 的连接跟踪条目数，对应 NET_CONNTRACK_ENTRIES
conntrack_entries = 8192

# 以下字段是 QEMU 设备/客户机网络，不是协议栈选择：
mac = "52:54:00:12:34:56"
backend = "socket,id=net,..."   # -netdev 规格
hostfwd = ["tcp::2222-:22"]
guest_ip = "10.0.2.15"
guest_netmask = "255.255.255.0"
guest_gateway = "10.0.2.2"
```

`stack` 与 `backend` 是两件不同的事：

- `stack` 选择**内核里运行哪个 TCP/IP 实现**；
- `backend` 是 QEMU 的 `-netdev` 规格（SLIRP、socket、tap……）。

## 2. 使用

```sh
# 推荐：实例驱动
tools/a20 build xfce-x86_64
tools/a20 run   xfce-x86_64

# 裸 make 也接受实例
make NET_INSTANCE=xfce-x86_64 kernel-only
make NET_INSTANCE=release-riscv64 dev-build

# 只想看会使用哪个构建目录
make -s NET_INSTANCE=xfce-x86_64 print-build-dir
```

优先级：命令行显式变量 > 实例 TOML > Makefile 默认值。因此可以在不改实例的情况下做一次性实验：

```sh
make NET_INSTANCE=xfce-x86_64 NET_STACK=lwip kernel-only
```

## 3. 校验规则

`tools/a20 check` 与 Makefile 都会拒绝不可能的组合：

| 组合 | 结果 |
|---|---|
| `stack = "bsd"` 且 `arch != x86_64` | 错误 |
| `stack = "bsd"` 且 `lanes != 1` | 错误（单执行器） |
| `stack = "bsd"` 且 `resource_profile != 2` | 错误（预算证据） |
| `stack = "bsd"` 且 `role = "router"` | 错误（未提供 router） |
| `stack = "lwip"` 且 `lanes != 1` | 错误（上游核心串行） |
| `stack = "none"` 且 kernel profile 不是 `mcu` | 错误 |
| `role = "router"` 且 `stack = "none"` | 错误 |
| `lanes < 1` / `resource_profile ∉ {1,2,3}` | 错误 |

这些规则和 Makefile 的 `$(error ...)` 是同一份语义；改一处必须改另一处。

## 4. 无静默回退

构建时：

- `CONFIG_NET_BACKEND_NAME` 被写成实例的 `stack` 字符串；
- 只有实际编译进来的后端会注册（`CONFIG_NET_BACKEND_LWIP` 或`CONFIG_NET_BACKEND_BSD`）。

启动时 `net_stack_init()` 按名字选择：

- 找到：调用 `cycle->init()`，`/proc/net/status` 显示真实后端名；
- 找不到：打印 `[NET] requested backend is not compiled in; network unavailable`，网络域保持不可用；socket 创建返回 `-ENETDOWN`。

不会自动换成另一个栈。这是有意的：换栈会改变窗口、缓冲区、PMTU 和错误语义，静默回退比没有网络更难诊断。

## 5. 迁移旧实例

未声明 `[net].stack` 时，Makefile 按用途选择默认值：

- 64 位普通带 MMU 的虚拟机、PC 桌面/服务器 → `bsd`
- 32 位、NOMMU、明确的嵌入式板卡 → `lwip`
- MCU → `none`

仓库中的全部 64 位非嵌入式实例已显式声明 `stack = "bsd"`。ARM32/RV32 普通虚拟机保留 `stack = "lwip"`。
`driver_deployment = "embedded"` 仅表示驱动静态打包，不改变用途分类。
`NET_INSTANCE` 同时传递 `NOMMU`，避免 NOMMU 实例错误继承普通虚拟机的默认后端。

## 6. 与资源档位的关系

`stack`（实现）与 `resource_profile`（预算）独立：

- 同一个 lwIP 可以跑 1/2/3 档；
- BSD 当前只验证了 2 档，因此实例若写 1/3 会在校验阶段失败；
- `lanes` 是执行并发度，不是 QEMU 拓扑，也不改变预算。

BSD 路由实例默认预留 4096 条连接。可以设置 `conntrack_entries`，允许 256 至 16384 的 2 的幂。该值独立于 socket 和 mbuf 的资源档位。

“避免重复 profile”的含义是：一份实例声明同时决定栈、并发和预算，后端不再各自维护一套私有的 `NET_PROFILE` 映射；后端只读取 `kernel/include/net/profile.h`中的统一常量。
