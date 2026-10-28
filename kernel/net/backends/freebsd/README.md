# FreeBSD 后端接入状态

上游以 git submodule 固定在 `upstream.toml` 的提交；未复制进 A20OS 跟踪源码。
当前只有来源固定与目录边界，**没有可运行或可参加性能排名的 FreeBSD 后端**。
不允许把 lwIP 标记成 FreeBSD，不允许缺失后端时静默回退。

初始化：`git submodule update --init --depth 1 kernel/external/freebsd-src`。
普通 lwIP/MCU 构建不要求此子模块存在。开发者可对该 submodule 使用 sparse-checkout，
只检出 `sys/net`、`sys/netinet`、`sys/netinet6` 和适配需要的核心头/实现；gitlink仍固定
整个上游提交。适配代码应留在本目录，不改写上游树。

接入必须落实：mbuf/DMA 所有权、UMA/分配预算、mutex/rwlock/epoch、callout/taskqueue、
VNET/网络域、socket缓冲和唤醒、接口/路由，以及 Linux ABI 选项/错误翻译。最终以真实
TCP/UDP、IPv6、转发与资源回收验收，而非以头文件可编译或符号可链接作为完成标准。
