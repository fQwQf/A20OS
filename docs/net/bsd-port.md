# BSD 网络后端接入记录（2026-10-29）

最新运行状态、XFCE 桌面联网证据、限制与复现命令见
[BSD 桌面联网垂直切片](bsd-desktop-internet-2026-10-29.md)。本页下文按阶段保留
历史记录，早期“未运行”描述以新记录为准。

初期已将完整 FreeBSD 源码子模块替换为独立项目
[F-Stack](https://github.com/F-Stack/f-stack)，初期固定提交
`52514377b89750ffc03a93f401c22d2d4f269d5f`（现已替换，见末节）。它将 FreeBSD 网络实现和用户态适配一起
维护，不要求导入整个 FreeBSD OS。另一个真实候选是
[libuinet](https://github.com/pkelsey/libuinet)，其 README 说明从 FreeBSD 9.1 起步。
本轮选择 F-Stack 作为接入起点，不根据其上游性能宣传推断 A20OS 性能。

## 实际探测结果

源码位于 `kernel/external/f-stack`；本地使用 sparse-checkout 读取 `lib/freebsd/mk`
相关目录，超级项目以 gitlink 固定版本，普通 lwip/native 构建不依赖其初始化。

```sh
git submodule update --init --depth 1 kernel/external/f-stack
make -C kernel/external/f-stack/lib -n all
```

本机第二条命令 exit 2：`lib/Makefile:65` 报未安装 DPDK。原始记录保存在
`build/evidence/net-upstream-20261029/f-stack-build-probe.log`。这是早期配置探测；后续独立对象编译见下节，尚无完整链接或数据面测试。`NET_STACK=bsd` 故意拒绝构建，避免静默回退。

## 需要替换的真实边界

| 上游入口 | 现有依赖 | A20OS 的接入要求 |
|---|---|---|
| `lib/ff_host_interface.c` | libc mmap、分配、pthread、时间与随机数、rte_malloc | 内核分配及预算；阻塞上下文和错误不能照搬宿主行为 |
| `lib/ff_thread.c`、`ff_lock.c` | 线程、TLS/pcurthread、同步 | 调度器线程上下文、锁顺序与每 CPU/任务状态；不能用全局假线程代替 |
| `ff_kern_timeout.c`、`ff_subr_epoch.c` | callout、epoch 及延迟释放 | 关闭与退出时停止回调，等待引用归还后回收 |
| `lib/ff_veth.c`、`ff_veth.h` | ff_dpdk_if_context、mbuf/DPDK 包释放和卸载 | 使用现有 virtio/TUN 设备；RX 只转移一次，TX 完成前不得释放 |
| `ff_syscall_wrapper.c`、`ff_api.h` | BSD socket/errno、线程上下文、sockaddr | 接入现有 `net/backend.h` 的 socket 模块；Linux IPv6 family=10，BSD=28，不能强转 |
| `ff_route.c` 与 BSD 路由表 | BSD 自身 FIB/接口索引 | 公共路由事务的执行副本；变更失败必须回滚，不能静默分叉 |

上游 `FF_KERNEL_COEXIST` 路径不能用作本项目的正确性替代，否则可能走宿主 Linux
socket。DPDK 不进入 A20OS 正常驱动路径；直接启用 DPDK 会破坏“相同驱动与链路”的
比较条件。也不能只实现分配器几个符号后把可链接当成可运行。

先贯通单线程 BSD 初始化、内存/同步/时钟，再适配 mbuf 与现有设备，随后接入 socket
生命周期、等待/错误和控制事务。每一步保留失败样本；最终运行与 lwip/native 相同的
Linux ELF，逐项比较转发/NAT、服务、吞吐、p99、CPU/字节与连接/内存。尚无 BSD 数值。

## 独立编译与部分链接进展

新增 `tools/probe-bsd-core.py --out <新目录>`，不调用依赖 DPDK 的上游完整构建，
直接编译选定的真实网络对象。所有生成头和机器目录软链接均位于输出目录；
`device_if.h`/`bus_if.h` 由上游 awk 生成器生成，不手写空壳、不修改 submodule。
使用 x86_64 freestanding、无 red zone、仅通用寄存器、large code model 和 BSD 内核头，禁用宿主
Linux 头/宏，不链接 libc/DPDK，也不以宿主 socket 替代协议执行。

`bsd-core9` 已编译46个对象并以 `ld -r` 部分链接：amd64 checksum、IPv4/IPv6 输入输出、
TCP 输入输出/连接生命周期、UDP、mbuf、socket/sockbuf/domain、接口、FIB/下一跳、
锁、callout 与 epoch。源码选择由 `kernel/net/backends/bsd/port-sources.toml` 拥有，
工具从 `upstream.toml` 读取版本，不在脚本中重复固定版本。checksum 按上游 VPATH 选择 amd64
实现，不能同时链接 portable 和 amd64 两份。早期缺生成头和重复定义的失败保留在
`bsd-core2`/`bsd-core4`；扩展 source list 时的路径错误保留在 `bsd-core7`。精确参数、源码 SHA、编译器、告警和未解析符号写入 manifest；工具及两个 TOML
文件也复制到证据目录，便于按原始 source list 重放。

部分链接仍有454个未解析符号，其中包括尚未加入的 BSD 对象与 A20OS 运行时适配，
不能把它们全部称为内核缺失 API。未提供成功空桩来隐藏这些依赖；目前没有 BSD 初始化、
TCP 连接或 KVM 运行结果，更没有 BSD 吞吐数字。后续需补齐 SYSINIT/VNET/PCPU、
分配与线程上下文、同步/callout/epoch，再接入现有 NIC/TUN 和 socket 执行表。


## 桌面/服务器目标与第一批运行时适配

桌面与服务器的最终后端现已指定为 BSD，见 [后端策略](backend-policy.md)。当前仍不
改变可联网的过渡默认，也不解除 `NET_STACK=bsd` 的构建拒绝；该选项不能用空操作表
或宿主 socket 实现假成功。

`bsd-host-runtime3` 扩展到51个上游对象及1个 A20OS 适配对象，52个对象均编译并部分
链接成功。新增真实 SYSINIT、UMA、BSD 时间管理与 F-Stack 初始化/glue；源码仍由
submodule 拥有。部分链接保留428个未解析符号，这不是428个都要重写的宿主 API。

`runtime/clock.c` 接通 callout 使用的 `ff_get_tsc_ns` 到 A20OS 单调时钟，消除对
`rte_rdtsc`/DPDK 频率和浮点换算的依赖；realtime 与 monotonic 使用不同的内核时钟。
`ff_get_current_time` 每次读取实际时间，不需要用户态轮询刷新共享缓存。此适配已经
与真实 `ff_kern_timeout.c` 同一部分链接，而不是只有未调用的接口定义。主机验证：

```sh
cc -std=gnu99 -Wall -Wextra -Werror \
  -Ikernel/include -Ikernel/external/f-stack/lib \
  kernel/net/backends/bsd/runtime/clock.c tools/tests/bsd_clock_host.c \
  -o /tmp/a20-bsd-clock-host
/tmp/a20-bsd-clock-host
```

命名空间隔离后的真实 clock 对象也与同一 harness 链接并通过，记录为
`bsd-host-runtime3/clock-host-test.json`。测试覆盖200天 uptime、纳秒进位、FAST/默认时钟语义、wall clock 回拨期间 callout
时间不倒退，以及 realtime 无刷新读取。它只验证该时钟适配，不是 BSD 启动测试。

工具同时产出 `bsd-core-isolated.o`：所有 BSD 的定义及未解析引用统一加
`a20_bsd_` 前缀，只有明确列出的两个 A20OS 时钟入口和 ELF GOT 特殊符号保留原名。
BSD `malloc/free/printf` 等不能误绑定到 A20OS 的同名、不同签名函数。未提供内存、锁、
线程等成功空桩。原始对象也保留，manifest 记录前后依赖、导入白名单以及 TLS 符号；
完整 readelf 符号、重定位和 section 输出可供下一步链接使用。

这次发现并保留5个 TLS 符号（包括未定义的 pcurthread）与 SYSINIT/VNET/PCPU linker
set 依赖。最终内核链接还必须为它们提供真实的执行上下文和正确的 section 生命周期。
不能把这些 TLS 变量改成共享全局，或让 BSD 访问用户进程的 FS/TLS。当前没有 BSD
KVM 启动、socket 或公网访问结果；lwIP/native 的公网成功不能转记给 BSD。


## 内存、任务 TLS 与发布基线修正

当前 submodule 为 F-Stack `v1.25`，固定提交
`761639943bdda33103aa98241ca6a3079f1c1b7e`，携带 FreeBSD 13.0 基线。
这是首次运行适配的开发基线，不代表已完成安全维护评估或生产发布验收。版本选择
来自实际检查：原固定提交和 v2.0 都携带 `ff_stub_14_extra.c`，其中说明为 link-only
空桩，并存在与真实源码不一致的接口签名。没有把该文件放进 A20OS 构建。
v1.25 不带该批次；新基线避免先围绕这些空桩补建运行行为。历史编译、失败和公网
证据全部保留；此前 lwIP/native 的成功仍不属于 BSD。

`bsd-release125-core-checked` 编译149个真实上游对象及3个运行时对象，部分链接成功，剩53个
未解析符号，包括 linker set、格式化/字符串、宿主服务与导入接口。该数字不能直接
解释为53个内核 API。`namespace_checked` 与 `no_native_tls_checked` 均为 true。
这些是编译和静态边界验证，不是 BSD 启动或联网验证。

### 内存适配

`runtime/memory.c` 替换 libc/DPDK 的 BSD malloc/UMA backing：

- heap 返回16字节对齐地址，匿名映射返回4096字节对齐且清零的内存。
- 仅接受空地址、私有匿名、读写映射；实际 backing 按页向上取整，按原始请求长度释放；不冒充 Linux mmap、共享映射或物理连续 DMA。
- 依据 `ff_glue.c` 的实际检查，映射失败返回 NULL，不能交给它 `(void *)-1`。
- 配额包含 backing 请求、私有头和对齐预留；embedded/default/server 分别为
  4/64/256 MiB。它不是已承诺能启动的最小配置，尤其 embedded 尚无 BSD 启动验收。
  不包含 slab 向上取整、静态 BSD 镜像或公共 socket/驱动的额外成本。
- 先原子预留预算，分配失败回滚；使用 `kmalloc_atomic`，不在 BSD 锁内触发回收或睡眠。
  realloc 失败保留原块及内容；unmap 长度不匹配不释放；heap/mapping 所有权混用立即报错。
- peak 是预留 backing 的高水位，包含进行中的分配；并发统计各字段独立采样。

宿主导入通过私有别名再映射，例如 `port_host_copy→memcpy`。
不能把 BSD 自身 `panic` 的定义改名到 A20OS 的同名入口；工具同时检查导入必须保持
未定义，防止将 BSD 定义覆盖到内核导出名。

### 任务独立 TLS

Clang 的 `-femulated-tls` 使真实 `__thread` 访问经过 `__emutls_get_address`，不触碰
用户 FS/TLS，也不把 pcurthread 改成共享假线程。注册表以 `proc_current()` 的实际任务
身份查找所有者；每个上下文最多32个变量、16 KiB arena，最多8个同时绑定的上下文。
这些是 runtime 上限，**不是 BSD 协议栈已支持8个并发执行器或独立网络域**。
F-Stack 的空锁宏仍要求第一次接入采用单执行器，IRQ 只入队，不能进入 BSD。

上下文先创建再由其任务绑定；停止回调、解绑后才能销毁。绑定中的上下文禁止销毁，
跨任务解绑拒绝，槽位耗尽会回滚所有权；未绑定时访问 TLS 立即失败。描述符保持只读，
首次访问从模板初始化并保证对齐；后续访问不分配、不修改共享索引。任务退出必须显式
解绑销毁，最终 BSD worker 的停止/失败路径仍待接通，不能声称生命周期已端到端验收。

编译探测可重跑：

```sh
python3 tools/probe-bsd-core.py --cc /path/to/clang --emulated-tls --net-profile 2 --out <新目录>
python3 tools/test-bsd-runtime-host.py --clang /path/to/clang --out <新目录>
python3 tools/test-bsd-runtime-host.py --clang /path/to/clang --sanitize --out <另一新目录>
```

本机使用 Debian Clang19.1.7，工具包解压在 `build/toolchains/bsd-clang19/`，未修改系统
工具链。证据为 `bsd-release125-runtime-host` 和最终的 `bsd-release125-runtime-final`（ASan/UBSan）：
内存对齐/清零/预算耗尽、后端分配失败、realloc 回滚、16,000次四线程分配释放通过；
真实 Clang TLS 访问器的初始化、64字节对齐、四任务隔离、宿主 TLS canary、跨任务
拒绝、8槽耗尽回滚、解绑回收通过，ASan/UBSan 无错误。预期的未绑定访问由子进程
验证为失败，不提供假成功。测试没有运行 BSD TCP/IP 状态机。

后续已经补齐宿主服务/linker set、持久执行器、`ff_veth`/virtio 数据面、公共 socket
出站/监听路径、动态 IPv4/MTU/link 事务、raw/PMTU、单 NIC `SO_BINDTODEVICE`、
netfilter/conntrack/NAT 桥、AF_PACKET TX/RX 以及 L3 TUN。TUN 通过
`protocol_tun.c` 的最小 FreeBSD ifnet 实现，地址仍走 `in_control`/`in6_control`，
入站包进入 `NETISR_IP`/`NETISR_IPV6`，而不是在 A20 层另起一套 IP 状态机。
socket 就绪改为每后端一个 kqueue，只对就绪 fd 做 `recv()`，空闲轮询不再随
socket 数量线性增长。`bsd_scale_test` 用 512 个空闲 UDP socket 加 32 包突发测量
`/proc/net/status` 的 `bsd_recv_probes`：全扫描回退为 16994 次，就绪集合为 64 次。

TUN 地址仍由 `SIOCSIFADDR`/`SIOCSIFNETMASK` 驱动。首次 `SIOCSIFADDR` 早于
`SIOCSIFNETMASK`，若把零掩码当 `/0` 安装，会生成 tun0 的直连默认路由并覆盖
a20n0 的默认路由；实现按类别补默认掩码，`tun_test` 还验证 off-link 目标不会
出现在 TUN 队列里。

`NET_STACK=bsd NET_LANES=1` 已在 x86_64 桌面完成 DNS/HTTP/HTTPS，并由真实
Firefox 151 读取 HTTP/HTTPS DOM。仍待完成：router 模式、外部 IPv6/RA、DHCP、
多队列/零拷贝数据面，以及与 lwIP 的同条件吞吐/p99 对比。x86_64 桌面/服务器已切换
为默认，其它架构保持 lwIP。详见
[BSD 桌面联网垂直切片](bsd-desktop-internet-2026-10-29.md)。
