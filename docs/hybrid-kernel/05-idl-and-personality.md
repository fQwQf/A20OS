# Native 服务 IDL 与 Linux 人格层

## IDL 当前状态

服务协议已从手写 proto 头迁移到 `user/svc/a20_services.idl`。
`tools/a20idl.py` 生成 `user/svc/a20_services_idl.h`，rtcd、svcman 和 ubd 的协议头只保留槽位/设备常量并 include 生成头。

已实现：

- 版本化常量与固定宽度 `message` 字段（生成结构体，wire layout 保持）；
- **版本化请求/响应信封**：所有服务消息携带 `{version, type, size}` 信封；rtcd 使用独立响应类型（`RTCD_REPLY_TIME/ALARM`），服务端 校验 version/size；`smoke-native-rtcd` 双向验证；
- **svcmgr/echod 协议 IDL 化**：`SVCMGR_REQ_ECHO/CRASH` 版本化消息 替换裸 echo 与魔法字符串，echod 校验版本并忽略畸形消息而非误 崩溃；`smoke-native-svc` PASS；
- `make check-a20-idl` 在 `a20os` conda 环境中重生成并比较活跃头 （本机无 conda 时需在配置好 `a20os` 的环境执行）。

registry 为内核 syscall 接口（0x0A03），不属于 channel 协议，不在 IDL 范围。剩余：双端绑定代码生成与动态版本协商。

## Linux 人格层

在 Native 原语上重建的 Linux 风格接口，两阶段已实现：

1. `a20_personality.h` — channel-backed pipe facade：跨消息字节流 （NONBLOCK drain 拼接）、部分读取保留剩余、level 触发的就绪 （pending 数据保持可读直到耗尽）；
2. `a20_linux.h` — 对象翻译层：fd 表（open/close/dup/read/write）、 匿名 mmap、pipe、socketpair、futex（ETIMEDOUT 映射）、epoll 风格 wait-many（共享 EventQ）；`smoke-native-linux` 六分区 PASS。

**语义对照**：`user/cmds/core/pipe_ref.c` 用真实 Linux pipe(2) 执行与 native 实现相同的序列（部分读、跨消息字节流、level 就绪）， `smoke-native-personality` 要求两个实现输出完全一致的 `PIPE_REF` 行——native 与 Linux ABI 语义对照成立。

剩余：fd 表 byte-stream 语义的完整覆盖、epoll level 触发通用化、更大测例集（CAgent 功能项）的语义 diff 与性能对照。本文档不把现有 Linux ABI 直通实现称为已完成的 starnix 人格层；Native ABI 仍是系统本体，完整人格负载是后续工作。
