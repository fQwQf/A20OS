# Linux Unix-socket ancillary messages

`SCM_RIGHTS` with `cmsg_len == CMSG_LEN(0)` is a valid empty descriptor list. Send the data normally, emit no received descriptor header, and continue parsing subsequent control messages. Retain existing malformed-length and maximum-descriptor checks.

Firefox's MiniTransceiver always creates an SCM_RIGHTS header, including replies with no handles. Rejecting that empty list with EINVAL makes the forkserver crash with `failed to send a reply message`, preventing content-process startup. This was identified with Firefox 151.0.3's libxul debug symbols at the deliberate null write in `ForkServer::RunForkServer`.

References:

- [Linux scm_fp_copy](https://github.com/torvalds/linux/blob/master/net/core/scm.c)
- [Firefox MiniTransceiver](https://github.com/mozilla-firefox/firefox/blob/main/ipc/glue/MiniTransceiver.cpp)
- [Firefox ForkServer](https://github.com/mozilla-firefox/firefox/blob/main/ipc/glue/ForkServer.cpp)

`scm_stress` covers empty lists on stream, datagram and sequenced-packet sockets, and an empty header followed by a nonempty header. The test passes on a Linux host. The preceding A20OS kernel reports `FAIL empty rights errno=22`; the fixed 4-CPU preemptible x86_64 and RISC-V kernels report `empty-rights PASS` and complete the descriptor-passing regression. The full x86 VM smoke also completes its memory, signal and socket probes (85 network-option checks).

Local evidence (generated logs, excluded from source control):

- `build/browser-followup/empty-rights-negative-guest.log`
- `build/browser-followup/empty-rights-full-guest.log`
- `build/browser-followup/empty-rights-rv-guest.log`

This fixes the demonstrated forkserver failure; browser rendering and longer interactive sessions require separate validation.

## 工作线程的凭据归属于进程

Unix socket 的 SO_PEERCRED、自动 SCM_CREDENTIALS 和 SO_PEERPIDFD 使用线程组 ID，不能使用发起 connect、bind、socketpair 或 send 的工作线程 ID。此前这些路径记录 task.pid；GLib 的 GDBus 工作线程可能早于进程退出，从而使服务端错误识别连接身份或认为对端已经退出。现在统一记录 task.tgid，包括 channel 数据通路与普通消息队列，没有架构分支。 Linux 对应约定见 [scm.c](https://github.com/torvalds/linux/blob/master/net/core/scm.c)。

`scm_stress` 的 process-credentials 子测试由工作线程创建 stream、datagram、 seqpacket socketpair，以及连接 stream/seqpacket 监听 socket，发送消息后退出。主线程验证 peer 和消息中的 PID 等于 getpid，并验证 peer pidfd 此时不可读。宿主 Linux 通过；旧内核在 `worker SO_PEERCRED must name process` 失败，日志为 `build/browser-followup/credentials-negative-guest.log`。修复后的四核抢占 x86_64 和 RISC-V 运行记录为`peer-pidfd-{x86,rv}-guest.log`，位于同一目录；另包含 85 项 NETOPT 回归。 x86_64 同轮 VM_FAULT 回归通过。桌面 D-Bus 的最终验收另见 XFCE 验证记录。

同轮修正 SO_PEERPIDFD 的 Linux 选项号：77（此前误写成 80），并让返回的 pidfd 带 FD_CLOEXEC。旧实现对 77 落入通用返回零的分支，并没有分配 pidfd，客户端可能把标准输入当作进程句柄。`pidfd-negative-guest.log` 在仅修正 TGID、尚未修正选项号的内核上复现失败；测试还验证返回的是新 FD 并带 CLOEXEC，防止只检查 poll 就误把不活跃的标准输入判为成功。 Linux 常量和实现见 [socket.h](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/socket.h)与 [sock.c](https://github.com/torvalds/linux/blob/master/net/core/sock.c)。
