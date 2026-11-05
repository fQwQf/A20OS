# BSD 后端接入状态

F-Stack 通过 submodule 固定于 `upstream.toml` 的 v1.25 提交。适配代码由本目录拥有，不修改上游工作树；构建阶段的少量源码变换在独立输出目录中执行，并校验匹配次数、记录变换前后的哈希。

```sh
git submodule update --init --depth 1 kernel/external/f-stack
```

BSD/F-Stack 是 **64 位非嵌入式实例**的默认后端，支持 x86_64、AArch64、RV64 和 LoongArch64。32 位普通实例、NOMMU 和嵌入式板卡保留 lwIP，MCU 默认 none。静态驱动部署不改变这个用途划分。BSD 构建不链接 lwIP，也不使用宿主 socket 代替内核协议栈。

运行路径使用单 BSD 执行器、外部 token/`ff_veth` 数据面和公共 Linux socket 接口。现有代码已接入多 NIC、TUN、router 角色、IPv4 配置与路由事务、TCP/UDP、raw socket、本地 IPv6、IPv6 地址事务及 A20 netfilter/NAT 桥。IPv4 网卡路径已验证分片 NAT。完整软路由验收还需要连接容量、PPPoE、IPv6 前缀委派、策略路由、服务恢复和长时间负载测试。具体进度见 [补全记录](../../../../docs/net/router-completion.md)，接口见 [网络后端接口契约](../../../../docs/net/backend-interface.md)。

`port-sources.toml` 列出真实 BSD 对象。`tools/probes/probe-bsd-core.py --arch <架构> --out <新目录>` 生成私有头文件、编译并部分链接，同时保留源码哈希、命令、告警、未解析符号与 ELF 检查结果。普通 BSD 构建自动执行此探测并链接隔离对象。

运行内核使用 Clang。x86_64、AArch64、RV64 采用 `-femulated-tls`；LoongArch64 通过显式访问器接入相同的任务 TLS 运行时。两种路径都按任务所有者分配和回收存储，避免使用用户 TLS 或共享全局变量。非 x86 端口使用本机直接映射、纯整数原子与校验和实现；RV64/LoongArch64 的私有 LP64 头文件布局从 arm64 personality 派生，并校验目标 ELF 机器类型。动态 BSD ELF 装载不在范围内，非 x86 目标明确返回 ENOEXEC。

四架构网络基础回归通过 `tools/net/tests/test-bsd-backend-qemu.py` 运行真实 A20OS 内核与 Linux ELF 测试程序，检查 TCP/UDP/IPv6 回环、512 个空闲 socket、四路外部 TCP、外部 UDP 和正常关机。构建测试内核使用 `NET_STACK=bsd NR_CPUS=2 RAMFS_USER=1 RAMFS_NET_TESTS=1`；AArch64 传入原始 `kernel.bin`，LoongArch64 额外使用 `UART_CMDLINE=y` 并传入 `kernel.elf`。这些检查不等同于完整桌面、软路由或真机性能验收。

历史桌面联网记录见 [BSD 接入记录](../../../../docs/net/bsd-port.md)。`BSD_BOOTSTRAP_OBJECT` 仅用于显式诊断，不是可用后端选择。
