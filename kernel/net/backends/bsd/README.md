# BSD 后端接入状态

选择独立项目 [F-Stack](https://github.com/F-Stack/f-stack)，通过 submodule 固定于
`upstream.toml` 的提交。已移除完整 `freebsd-src` 子模块。适配代码由本目录拥有，
不修改 F-Stack 源码。它携带自己的 FreeBSD 网络子树，不代表完全没有第三方源码。

```sh
git submodule update --init --depth 1 kernel/external/f-stack
```

**x86_64 host/desktop 垂直切片已运行。** `NET_STACK=bsd NET_LANES=1` 编译真实
F-Stack 核心，使用单 BSD 执行器、外部 token/`ff_veth` 数据面和公共 Linux socket
完成 DNS/HTTP/HTTPS；真实 XFCE 桌面 curl 已验证。当前支持单 virtio、动态 IPv4/MTU/link/邻居、TCP/UDP、本地 IPv6、
raw socket、IP_PKTINFO/IP_SEND*、PMTU、TCP_CONGESTION、IPv6 地址事务、
SO_BINDTODEVICE（单 NIC 语义）和 A20 netfilter/NAT 桥；TUN、router、
外部 IPv6/RA、DHCP 和完整关闭/性能验收仍缺失。这里
不链接 lwIP，也不调用宿主 socket 冒充 BSD。后端接口见 [网络后端接口契约](../../../../docs/net/backend-interface.md)，
接入细节见 [BSD 接入记录](../../../../docs/net/bsd-port.md)。

具体边界、源码入口和验收顺序见 [BSD 接入记录](../../../../docs/net/bsd-port.md)。

`port-sources.toml` 明确列出当前独立编译的真实 BSD 对象。运行
`python3 tools/probe-bsd-core.py --out <新目录>` 可生成头文件、编译并部分链接，
保留逐对象源码哈希、命令、告警与全部未解析符号。当前177个对象通过。普通 BSD
构建会自动重跑该探测并链接隔离对象；运行时能力边界以桌面联网记录为准。

当前移植基线为 F-Stack `v1.25`，固定提交见 `upstream.toml`。原先选定提交及 v2.0
包含仅用于链接的 FreeBSD-14 空桩批次，未将这些空桩引入内核。新基线仍需独立验证，
不能把上游 release 标签当成 A20OS 桌面/服务器的可发布保证。

运行格式使用 Clang `-femulated-tls`，通过任务所有者查找独立存储，避免读写用户
FS/TLS。原生 TLS 的 GCC 编译仍只用于依赖探测，不是内核运行格式。内存与 TLS 的
配额、生命周期及测试命令见 [BSD 接入记录](../../../../docs/net/bsd-port.md)。

运行状态与复现入口参见 [BSD 接入记录](../../../../docs/net/bsd-port.md) 及 [网络协议栈状态](../../../../docs/status/current/net-status.md)。
BSD 已在真实 A20OS KVM 中执行初始化、TCP/UDP 回环、公共 Linux socket ABI
和公网 HTTP/HTTPS；`BSD_BOOTSTRAP_OBJECT` 仅是显式诊断，不是可用后端选择。
物理板卡、TUN/router 和完整性能验收仍未完成。
