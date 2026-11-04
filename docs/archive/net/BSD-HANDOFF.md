# BSD 网络接入交接（2026-10-29）

本文件保留 2026-10-29 中段的交接基线。**后续已完成第一个真实桌面垂直切片：x86_64 XFCE 使用 BSD 后端、virtio-net 和公共 Linux socket 完成 DNS/HTTP/HTTPS，并由真实 Firefox 151 读取 HTTP/HTTPS DOM；动态 IPv4/MTU/link 事务也已通过。**当前状态、限制和复现见[BSD 桌面联网垂直切片](bsd-desktop-internet-2026-10-29.md)。

这仍不是完整 BSD 后端：外部 IPv6/RA、DHCP、TUN/router、raw/PMTU、完整错误/关闭回收和性能矩阵未完成；x86_64 桌面/服务器默认已是 BSD，其它架构仍为 lwip。不要把本页较早的“未运行”描述当作最新状态。

工作分支：`codex/net-backends-router`。最近实现提交：

| 提交 | 内容 |
|---|---|
| `d0776022` | 有预算的 BSD backing、任务独立 TLS、v1.25 开发基线 |
| `49b84500` | 真实 BSD SYSINIT/UMA、宿主服务、linker sets、主机 UDP/TCP 回环 |
| `a0dfb0b2` | 将同一 freestanding BSD 对象链接进 A20OS，执行 KVM 回环诊断 |

## 当前能做什么

| 路径 | 已验证 | 未验证/未实现 |
|---|---|---|
| 默认 `NET_STACK=lwip` | 未修改 upstream 2.2.1；此前冻结版本真实公网 HTTP/HTTPS 通过 | 最新收尾版本仅重新构建，未再次跑公网测试 |
| `NET_STACK=native` | 衍生协议实现，此前冻结版本公网验证通过 | 本轮没有重测 |
| BSD 私有运行时 | 分配/回收/预算/TLS 测试及 ASan/UBSan 通过 | 完整 BSD worker 退出、预算耗尽时 BSD 上层行为 |
| BSD 协议核心 | 真实 A20OS KVM 执行 SYSINIT、UMA；单 virtio、公共 socket、DNS/HTTP/HTTPS、IPv4/IPv6 listen/accept、动态 IPv4/MTU/link；XFCE 桌面 curl 与 Firefox DOM 通过 | 外部 IPv6/RA、DHCP、TUN/router、raw/PMTU、完整 close/cancel/低内存 |
| `NET_STACK=bsd` | x86_64 desktop/server/benchmark 默认，`NET_LANES=1` | 非 x86_64、router/embedded、IPv6/DHCP/TUN/raw、完整能力/性能验收 |

**KVM 诊断没有网卡，使用 `-net none`。** 它在初始化内核任务里运行 BSD 私有 socket API，随后保留已初始化域并循环让出 CPU，不进入用户 init。该诊断构建仍标记为 lwip；诊断没有经由它发送回环流量。普通 Linux ELF 经公共前端使用 BSD 的路径尚未接通。

## 基线、边界和代码入口

F-Stack submodule：`kernel/external/f-stack`，release `v1.25`，提交`761639943bdda33103aa98241ca6a3079f1c1b7e`，FreeBSD 13.0 基线。源码保持未修改。此前固定提交及 v2.0 带 `ff_stub_14_extra.c` 链接空桩批次，未引入该文件。 v1.25 是开发基线，不是生产安全维护已经通过的保证。

| 入口 | 作用 |
|---|---|
| `kernel/net/backends/bsd/port-sources.toml` | 169个上游对象、3个 A20OS 对象、1个 BSD 私有适配对象；另生成1个 linker interface 对象 |
| `runtime/memory.c` | heap 16字节对齐；映射向上按4096字节取整并清零，按原始长度释放；atomic backing 分配/预算/回滚 |
| `runtime/tls.c` | Clang emulated TLS；以实际 `proc_current()` 任务身份绑定；描述符只读，无用户 FS/GS 访问 |
| `runtime/services_bsd.c` | 私有环境、BSD格式化、日志、随机数导入、单队列默认 RSS 路径、启动配置 |
| `sets.ld` | 保留真实 SYSINIT/sysctl/module/per-CPU sets；A20OS 和 BSD 符号隔离 |
| `bootstrap_test.c` | A20OS 内核诊断入口；只在显式诊断构建启用 |
| `tools/probe-bsd-core.py` | 固定版本/洁净检查、生成头、编译/部分链接、namespace/TLS检查、完整依赖清单 |
| `tools/test-bsd-bootstrap-host.py` | 执行真实 BSD 初始化及协议回环；生成不含宿主 shim 的 `kernel-bootstrap.o` |
| `tools/test-bsd-bootstrap-kvm.py` | 启动实际 A20OS KVM，要求初始化与 UDP/TCP 三个 marker |
| `kernel/include/net/stack.h`、`transport.h` | 下一步需要实现的公共后端操作表 |
| `kernel/net/core/stack.c` | 当前每内核固定一个后端的分派，不支持运行中迁移连接 |

编译模型使用 `-mcmodel=large -fPIC -mgeneral-regs-only -femulated-tls`，无 libc/DPDK。 PIC 是必需的：Clang 对自定义 per-CPU section 的非 PIC 引用曾产生32位绝对重定位，无法落在 A20OS 的 `0xffff8000...` 地址。x86 linker script 已显式收集 `.ltext/.lrodata/ .ldata`；原有 `.lbss` 收集保留，避免 orphan section 落到物理分配器边界以外。

所有 BSD 符号（含未解析引用）有 `a20_bsd_` 前缀；只有明确 host imports 映射至内核。不要将 BSD `panic/malloc/free` 等同名不同签名函数直接绑定到内核符号。`asprintf/strdup` 使用 BSD kernel 签名，不能按 libc 签名调用。

上游 `ff_compat.c` 的 seeded `rand_r` 版 arc4random 在**生成对象**中显式改名，真实导出由服务适配直接调用 OS 随机数接口；变换记录在 TOML/manifest，不修改 submodule。这没有证明 A20OS `random_fill` 具备生产所需的密码学强度；应审阅 `kernel/core/random.c`。

## 已验证证据

证据根：`build/evidence/net-router-completion-20261029/`。这些目录被 Git 忽略，交接到另一台机器时需单独复制，或者从提交的工具重新生成。输出目录必须是新的目录。

| 目录/文件 | 结果 |
|---|---|
| `bsd-bootstrap6/manifest.json` | 174个对象成功；namespace 与无原生 TLS 检查通过；22个未解析引用为 host imports/GOT/linker sets，最终链接已解决 |
| `bsd-boot-loopback7/boot.log` | 真实 BSD 初始化、UDP/TCP 双向回环成功 |
| `bsd-boot-loopback7/kernel-bootstrap.o` | 不含 Linux host hooks/libc 的内核诊断链接对象 |
| `bsd-handoff-runtime-asan/` | backing 分配/失败回滚/对齐/并发释放；TLS并发/所有权/槽耗尽/回收；ASan/UBSan通过 |
| `bsd-handoff-kvm/manifest.json`、`serial.log` | KVM `passed=true`，BSD内核回环成功；backing/peak=1,426,020 B，limit=67,108,864 B |

KVM 冻结内核 SHA256：`b2cda775fd1e0f1134f62bef60cbf6759fe4de2be316505badcc0b2a9d0a2b32`。对应构建目录 `.kernel-build/x86_64-qemu-virt-x86_64-both-297d41afd799ea74/`。 backing 约1.36 MiB，仅是此诊断域的预算统计，不含 slab 取整、静态镜像、公共前端和驱动，也不是连接规模或峰值吞吐负载的内存成本。

普通 lwip 构建收尾通过，日志 `/tmp/a20-bsd-handoff-normal-build.log`，输出目录`.kernel-build/x86_64-qemu-virt-x86_64-both-535534f24b63854b/`。它没有启用 BSD 诊断。上游洁净/公共边界检查见下方命令。

已保留失败：非整页 UMA 映射被拒绝导致启动失败、把 `_DYNAMIC` 设成零地址被编译器视为非空对象而崩溃、非 PIC 的32位重定位溢出、诊断宏早于 CFLAGS 定义被覆盖。现在映射实际取整、静态镜像提供真实 DT_NULL 终止表、使用 PIC、宏在 CFLAGS 定义后追加。`link_elf_lookup_symbol: missing symbol hash table` 仍是已知静态镜像诊断信息；当前依赖真实 linker-set fallback，未实现外部 BSD KLD 加载，不要声称这部分已兼容。

## 复现

本机 Clang19.1.7 位于 `build/toolchains/bsd-clang19/root/usr/lib/llvm-19/bin/clang`；来自 Debian 的 clang-19/libclang-cpp19 包，仅解压在 workspace，没有修改系统工具链。另一台机器可使用支持 `-femulated-tls` 的兼容 Clang，并准备 GNU binutils、awk、 Python3.11+、现有 A20OS x86_64 编译环境和 KVM。

```sh
git submodule update --init kernel/external/f-stack kernel/external/lwip-upstream
CLANG="$PWD/build/toolchains/bsd-clang19/root/usr/lib/llvm-19/bin/clang"
R="$PWD/build/evidence/bsd-repro-$(date +%Y%m%d-%H%M%S)"
python3 tools/probe-bsd-core.py --cc "$CLANG" --emulated-tls --net-profile 2 --out "$R/core"
python3 tools/test-bsd-bootstrap-host.py --core "$R/core" --out "$R/host"
make -j8 ARCH=x86_64 NR_CPUS=2 NET_STACK=lwip NET_LANES=1 OPT=-O3 CONFIG_UBSAN=0 \
  BSD_BOOTSTRAP_OBJECT="$R/host/kernel-bootstrap.o" kernel-only
KDIR=$(make --no-print-directory ARCH=x86_64 NR_CPUS=2 NET_STACK=lwip NET_LANES=1 \
  OPT=-O3 CONFIG_UBSAN=0 BSD_BOOTSTRAP_OBJECT="$R/host/kernel-bootstrap.o" print-build-dir)
python3 tools/test-bsd-bootstrap-kvm.py --kernel "$KDIR/kernel.elf" --out "$R/kvm"
python3 tools/test-bsd-runtime-host.py --clang "$CLANG" --sanitize --out "$R/runtime-asan"
python3 tools/check-network-backend-boundary.py --check-upstream
```

`BSD_BOOTSTRAP_OBJECT` 是显式诊断入口，内容哈希参与构建目录选择；不会影响普通构建。该诊断不能代替 `NET_STACK=bsd`。日常构建请不传此变量。

## 接手后的工作顺序与已知约束

> 2026-10-29 后续：第 1、2 项的 x86_64 单 NIC/静态 IPv4 部分及第 3 项的出站 socket 子集已落地，并完成桌面 HTTP/HTTPS。以下列表继续用于剩余工作。

1. 建立持久 BSD executor 的启动/停止契约。当前诊断复用了初始化内核任务；正式后端应有稳定所有者，绑定 TLS 后执行一次初始化，IRQ只入队，不能在任意 Linux 调用线程和中断里同时进入 BSD。F-Stack 的空锁宏不是 A20OS SMP 锁实现。8个 TLS槽也不等于支持8个并行 BSD 栈实例。`_sleep/pause/M_WAITOK` 等上游路径需审计，达到 backing预算不等于 BSD 调用者能正确处理资源耗尽；不能用成功空桩绕过。
2. 接入现有 virtio 包 I/O、BSD ifnet/mbuf、RX/TX所有权和背压。公共 driver/stack 分派已存在，不应再让 virtio/e1000/rtl8139 包含协议私有头。先双向收发与 ARP/ND，再上 DNS/TCP。TUN 同样要实现公共索引和同步借用接口，不传 pbuf/mbuf 给前端。
3. 实现 `net_backend_t`（`cycle/event/socket` 必备，其余按能力补齐），包括 socket 生命周期、非阻塞进展、接收/accept 发布、close与回调撤销、接口/路由配置事务。BSD errno、IPv6 family28、 sockaddr长度字节、私有 fd 都不能直接泄漏为 Linux ABI。公共前端必须完成用户内存校验/复制，再把受控内核缓冲交给 BSD；不能直接照搬 F-Stack 的 memcpy式 copyin。
4. 推进时钟/callout、netisr、epoch/taskqueue，按 `hz` 的真实时间间隔推进，不能每次 poll 都调用一次 hardclock。当前短时回环不验证重传/超时、长时间运行或 teardown。
5. 在相同驱动与链路下让同一 Linux ELF 使用 BSD，验证 DNS、公网 HTTP/HTTPS、在线 apk 和 IPv4/IPv6/ABI；再跑吞吐、p99、CPU/字节、连接规模与峰值/回收内存。成功后才把普通默认扩展到更多架构/资源档。x86_64 桌面/服务器默认已切换。
6. 原始范围中仍有完整分片 NAT、本机 socket PMTU/错误队列、软路由整体验收；前轮 ICMP RELATED/转发MTU错误的成功不能代替这些工作。详见 `../../net/nat.md` 和`router-completion-2026-10-29.md`。

未创建 PR、未推送、未切换默认后端、未联系接手者。交接时以 Git 提交及本文件为入口。
