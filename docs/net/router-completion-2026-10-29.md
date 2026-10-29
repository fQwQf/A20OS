# 网络解耦与路由错误回归（2026-10-29）

本轮运行内核对应 `70f7534a`。`lwip` 使用未修改的 upstream 子模块；`native` 使用
独立拥有的衍生实现；BSD 仍在移植，不能启动。以下通过项仅适用于实际运行的两个后端。
证据根为 `build/evidence/net-router-completion-20261029/`，失败日志保留。

## 能力与边界

| 项目 | 本轮实现和实际验证 | 仍缺少的范围 |
|---|---|---|
| TUN | 公共层无 lwIP 类型；后端拥有接口/包生命周期；持久化、复用、配额、IPv4/IPv6 收发 | PI/VNET/GSO 完整互操作矩阵、多队列、delegated owner/group |
| socket ABI | IPv4/IPv6 UDP 显式/隐式 bind、connect、源地址/端口与线上值一致 | 不能据此宣称所有 Linux socket 选项兼容 |
| NAT RELATED | SNAT/MASQ/DNAT 对 ICMP 差错中的地址/端口及校验和翻译；8字节 TCP 最短引用 | 完整 IPv4 分片重组/NAT，IPv6 NAT |
| 转发 PMTU | IPv4 DF→需要分片/正确 MTU；IPv6 Packet Too Big；两族 TTL/hop-limit 错误 | A20OS 本机 socket 完整 PMTU 缓存和错误队列策略 |
| BSD | 真实46个协议/socket/路由对象独立编译、部分链接，无 DPDK/libc | SYSINIT/VNET/PCPU、运行时/设备/socket 接入，全部运行回归 |

有 NAT 规则时 IPv4 分片在改写或建立 binding 前拒绝，防止只翻译首片而泄漏尾片。
这是安全边界，不是分片 NAT 支持。下一步应在公共包所有者中实现有预算、超时、重叠
拒绝和网络域隔离的完整数据报重组，再执行 policy/CT/NAT，最终按出口 MTU 分片。
不能用仅记忆首片端口的无界缓存代替，也不能为此重新修改上游 lwIP。

## 三节点 KVM 回归

最终证据目录为 `lwip-commit70f-kvm` 与 `native-commit70f-kvm`，两者
`manifest.json` 的 `passed=true`。Linux LAN→A20OS 双网卡→Linux WAN，所有节点
2 vCPU/2 GiB，virtio 卸载关闭；CPU 分别为8,10 / 4,6 / 12,14。QEMU socket 链路
无需修改宿主 TAP、路由或防火墙。原版 lane=1，native lane=4；这是各自执行模型的
正确性回归，不能称为相同并发度的协议核心比较。

- 同一 TUN/socket ELF 在 A20OS 路由节点和 Linux WAN 各运行一次。检查接口配置、
  MTU 查询不改变 UP 状态、持久化后重接、全部槽位保留后重接、24轮创建/销毁、
  readonly ifreq 的 EFAULT 回滚，以及64次独立 UDP 包队列绕回。
- IPv6 普通 UDP、显式 bind/connect、隐式 send、隐式 connect；检查 getsockname
  的源地址/端口与 TUN 实际报文一致。Linux DAD 根据真实地址就绪重试。
- 同一 ICMP probe ELF 在 Linux LAN 使用 IP_RECVERR/IPV6_RECVERR 检查：IPv4
  NAPT 后外部端口不可达、TTL 超时、DF/MTU=1280，以及 IPv6 端口不可达、hop-limit
  超时、Packet Too Big/MTU=1280。检查错误类型/代码、来源、原目标及可见引用数据。
  这些错误队列属于 Linux LAN，不能冒称 A20OS 本机错误队列已实现。
- 保留既有同源端口 NAPT 冲突/回复归属、策略路由正负例、IPv4/IPv6 转发、DHCP
  广播租约和 DNS 查询。PCAP 独立重算 IPv4/TCP/UDP 校验和，四个抓取点均无错误；
  本轮 PCAP 没有分片，不能用它证明分片正确性。IPv6 错误由 probe 验证。

冻结 ELF 的 SHA256：

| 用途 | SHA256 |
|---|---|
| TUN/socket probe | `8116392d71cec4a1ccc377c53ff4c14d042f08134d8d857a8da9dd48a55e3b9f` |
| ICMP probe | `7ec9bff076f13c8e8b308ee58be856b1a83cdb09725af4886f025275764fafc0` |
| 路由 microbench | `85b636db904bc69c7df83648d9c97a2e57e5a548886d84a78ba8b162e122f166` |
| lwip 内核 | `bb2af1a900998728306aa27264671f2ab591244f29d3215a78fb707c72dfb2c8` |
| native 内核 | `6399e32722e704be51f0f9a7f3704b06fb76a7f0de9e92d038f91b8aff651d11` |

可重跑命令（使用新的输出目录，按需替换后端）：

```sh
E=build/evidence/net-router-completion-20261029
python3 tools/test-net-router-kvm.py \
  --baseline build/evidence/net-refactor-20261028/runtime-smoke/manifest.json \
  --router-kernel "$E/kernel-lwip-70f7534a.elf" \
  --binary build/evidence/net-refactor-20261028/router-binary/net-microbench \
  --socket-probe "$E/tun-probe9/net-tun-abi-probe" \
  --icmp-probe "$E/icmp-probe4/net-icmp-router-probe" \
  --linux-preload-module "$E/tun.ko" \
  --services-world build/evidence/net-refactor-20261028/router-services-input2/world.img \
  --dhcp-broadcast --out "$E/new-lwip-run"
```

`linux-preload-module` 仅装入固定 Linux **客体**，必须与其内核版本一致；不会装入宿主。
串行运行两个后端，或指定不同的 LAN/WAN 监听端口。manifest 的 `inputs` 保留基准
材料身份；实际覆盖的路由内核以节点 `-kernel` 和 `router_sha256` 为准，不能误读
基准 manifest 中历史 `inputs.a20_kernel`。普通性能使用的 ELF 与路由工具版本不同，
分别固定哈希，在各自的系统比较内部保持同一 ELF。

## 主机验证与修复线索

生产 C/Rust NAT harness 的 default、embedded、2 GHz tick、四线程并发均通过。
`icmp-indexed-host` 检查 SNAT/DNAT × TCP/UDP × 最短/完整引用 × 三类差错，包含
独立校验和验证、所有截断前缀不得改变输入、MTU 保留与分片不得建立 CT 状态。
RELATED 查找使用已有 original/reply 索引，没有在数据面扫描整个 CT 表。
C/Rust rewrite 47项检查及 UDP send Rust 14项检查通过。原版/native 的正常构建与
embedded 构建、26文件公共边界/上游洁净检查、并发接线检查通过。

失败样本揭示并修复了真实问题：TUN FIFO 出队后写入位置错误、隐式 UDP 源端口
撞上显式绑定、IPv6 UDP 拒绝路径、连接后源地址发布错误、ifreq IPv4 地址偏移和
错误的 TUNSETPERSIST ioctl 编号。它们不是通过放宽 probe 消除。另有测试环境问题：
Linux TUN 模块缺失、DAD 未完成、RS 控制包被误认作 UDP、前序用例预先学习 PMTU、
并行运行监听端口冲突；相应失败目录保留，不混入最终通过结果。

性能与当前容量限制见 [对照方法](benchmark-method.md)。BSD46对象部分链接仍有454个
未解析符号，无运行数据，见 [BSD 接入记录](bsd-port.md)。

## 实际公网访问补充验证

`internet-check/` 使用同一组70f7534a冻结内核，分别启动 lwip/native KVM，经 virtio +
QEMU user/SLIRP、静态地址10.0.2.15、网关10.0.2.2、DNS10.0.2.3访问公网。
Alpine BusyBox 在两个 A20OS 客体中均成功解析 example.com，分别下载
`http://example.com/` 和 `https://example.com/`；四次 wget 全部退出0，得到577字节
页面，均检出 `Example Domain` 标记。脚本、runner、QEMU参数、内核/脚本哈希和完整
串口记录均保留。未给客体注入宿主 HTTP/SOCKS 代理。这验证基本 DNS/TCP/HTTP/TLS
下载，不覆盖完整浏览器、所有 TLS 验证行为或在线 apk 安装全流程，也不等于下游设备
经 A20OS NAT 到公网的整体验收。双网卡 NAT 的已验证范围仍见前文。

注意 `/proc/net/config` 的 `tcpmode=lwip` 是历史软件快路径/协议路径选择项，
**不是编译后端的身份**；native 同样使用该配置值。后端身份应读取
`/proc/net/status` 的 `backend` 字段或检查冻结内核及其构建配置。
