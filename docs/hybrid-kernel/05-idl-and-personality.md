# Native 服务 IDL 与 Linux 人格层

## IDL 当前状态

服务协议常量已经从手写 proto 头迁移到
`user/svc/a20_services.idl`。`tools/a20idl.py` 生成
`user/svc/a20_services_idl.h`，rtcd、svcman 和 ubd 的协议头只保留
槽位/设备常量并 include 生成头。

当前语法故意只覆盖版本化常量。下一步是增加 `message` 字段描述、
大小/对齐规则和双端绑定生成；在此之前不得把手写结构体声称为完整 IDL。

当前迁移范围包括 rtcd、svcman 和 ubd 的请求常量；`make check-a20-idl`
会在 `a20os` conda 环境中重生成并比较活跃头。由于本机当前没有可用的
`conda` 命令，该门禁需在配置好 `a20os` 的环境执行。

## Linux 人格层第一块

Linux ABI 的完整人格层仍未完成；它要求把 fd、pipe、mmap、epoll、futex
和 socket 语义建立在 Native 对象上，并与当前内核直通实现做语义 diff。
当前保留的验证顺序是：Native 原语契约先通过，再用 channel/EventQ/VMO
实现用户态 pipe 子集，随后对读写、关闭、背压和就绪测试做两种实现对照。

本文档不把现有 Linux ABI 称为已完成的 starnix 实现。Native ABI 仍是
系统本体，Linux 兼容层仍是后续人格负载。
