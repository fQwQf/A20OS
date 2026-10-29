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
