# BSD 后端接入状态

选择独立项目 [F-Stack](https://github.com/F-Stack/f-stack)，通过 submodule 固定于
`upstream.toml` 的提交。已移除完整 `freebsd-src` 子模块。适配代码由本目录拥有，
不修改 F-Stack 源码。它携带自己的 FreeBSD 网络子树，不代表完全没有第三方源码。

```sh
git submodule update --init --depth 1 kernel/external/f-stack
```

**当前未运行。** `NET_STACK=bsd` 明确报错；不链接 lwIP、不调用宿主 socket 冒充 BSD。
实际检查上游 `lib/Makefile` 发现 DPDK 依赖；`make -n` 配置探测亦因此失败。
这不是“安装 DPDK 后即可成为内核后端”：host 内存、线程/TLS、时钟、随机数以及
veth/mbuf 的释放和驱动接口都需适配到 A20OS。

具体边界、源码入口和验收顺序见 [BSD 接入记录](../../../../docs/net/bsd-port.md)。

`port-sources.toml` 明确列出当前独立编译的真实 BSD 对象。运行
`python3 tools/probe-bsd-core.py --out <新目录>` 可生成头文件、编译并部分链接，
保留逐对象源码哈希、命令、告警与全部未解析符号。当前149个上游对象与3个 A20OS 运行时适配对象通过；这尚不是
`NET_STACK=bsd` 的可运行构建清单，不能据此声称初始化、收发或 socket 已贯通。

当前移植基线为 F-Stack `v1.25`，固定提交见 `upstream.toml`。原先选定提交及 v2.0
包含仅用于链接的 FreeBSD-14 空桩批次，未将这些空桩引入内核。新基线仍需独立验证，
不能把上游 release 标签当成 A20OS 桌面/服务器的可发布保证。

运行格式使用 Clang `-femulated-tls`，通过任务所有者查找独立存储，避免读写用户
FS/TLS。原生 TLS 的 GCC 编译仍只用于依赖探测，不是内核运行格式。内存与 TLS 的
配额、生命周期及测试命令见 [BSD 接入记录](../../../../docs/net/bsd-port.md)。
