# dbus 同步调用超时 / 对端 pid 为 -1

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[DESKTOP-12](../desktop.md)。

会话里 `NoReply`，xfsettingsd/panel 报 Xfconf/dconf 超时后降级；dbus 日志里身份异常：
`dbus-daemon[44]: [session uid=0 pid=18446744073709551615 pidfd=5] ...`（pid 为 -1）。在 `e2fsck`
干净的 rootfs 上稳定复现。同一行里 `requested by ':1.7' (uid=0 pid=92 comm=".../xfsettingsd")` 的 pid
却是对的。

SO_PEERCRED 那一半已修：`kernel/net/socket_unix.c` 的 connect 路径把发起端自己的 pid 写进了
`s->peer_pid`，而发起端的对端应是服务端。现在 bind 时记录属主凭据（`net_socket_t.owner_pid/uid/gid`），
connect 时用它给发起端填 `SO_PEERCRED`；accept 出的子 socket 那一路原本就对。

仍未修的是 pid 本身：改完后 dbus 日志里的 `pid=18446744073709551615` 依旧。同一行 `pidfd=5` 说明
`SO_PEERPIDFD` 已经拿到 pidfd，所以 dbus 显示的这个 `pid=` 不是取自 `SO_PEERCRED`，而是 `SO_PASSCRED`
的 `SCM_CREDENTIALS`（`ch_cred_*` 路径）。下一步查 AF_UNIX 首包（dbus 的初始 NUL 字节）的
SCM_CREDENTIALS 为什么是 -1。已排除 `peer_pid` 未初始化（`net_socket_alloc()` 用
`obj_cache_alloc_zero()`）与 `getpid()` 本身（`dbus-daemon[44]` 说明任务 pid 正确）。
