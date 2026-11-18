# UDP 扩展错误队列

为 CLI 的 dig/nslookup 提供 `IP_RECVERR` / `IPV6_RECVERR`、`POLLERR` 和 `recvmsg(MSG_ERRQUEUE)` 路径。当前范围为 BSD 后端、初始网络命名空间、直接 IPv4/IPv6 UDP 的 ICMP 错误；不支持的后端、命名空间或 socket 类型返回错误。

## 接口行为

- 设置与查询返回实际启用状态。禁用会清空扩展错误队列。
- ICMP 端口不可达等错误进入独立队列；普通接收数据不被错误记录替代。队列最多 8 条，数据与元数据受单独的 native `SO_RCVBUF` 字节预算约束，不等同 Linux skb 记账。
- 错误使 poll 报告 `POLLERR`，与调用者请求的事件掩码无关。`SO_ERROR` 与普通 UDP 接收可以消费 pending errno，扩展队列仍保持可读；从扩展队列取走记录后按剩余记录更新 pending 状态。
- `recvmsg(MSG_ERRQUEUE)` 不阻塞，空队列返回 EAGAIN。payload 为 ICMP 实际引用的原 UDP 数据前缀，名称是原目标端点；控制消息包含 16 字节 `sock_extended_err` 和 offender 地址。
- 返回标志包含 `MSG_ERRQUEUE`；数据或控制容量不足分别报告 `MSG_TRUNC` / `MSG_CTRUNC`。零 iov 可以消费控制消息，零名称容量不写名称缓冲区但返回实际地址长度。用户复制失败也可能已经消费记录。
- `recvfrom(MSG_ERRQUEUE)` 消费同一队列，返回被引用的数据及目标名称。最后一个 socket 引用释放时回收剩余记录。

BSD 执行器拥有每个地址族一个有界、常驻的 raw ICMP reader，通过 kqueue 按需调度。原始报文已由 BSD 栈完成 IP/ICMP 检查；适配层有界解析被引用的 UDP 头，并匹配后端的本地端点、已连接 peer 和绑定设备。遍历目标时持有 socket 引用，记录进入前端队列后不保留 BSD 数据指针。

## 已验证范围

2026-11-16，RV64 QEMU virt、ABI=both、BSD、SMP2 的 `cli-application/udp-error-queue-final/report.json` 为 PASS：IPv4/IPv6 回环真实 ICMP 端口不可达、独立错误消费、普通接收错误、短缓冲区、零 iov/名称容量和禁用清队列均通过；实际 dig/nslookup 收到宿主本地 UDP DNS 的答案。正常关机及 ext4/FAT 只读检查退出 0。相同错误探针在宿主 Linux 通过；生产解析器/队列有 1 项主机检查，poll/recv 各 12 项检查通过。

原始报告位于 `build/evidence/v1-closeout-20261116/`，入口为 [CLI 应用探针](https://github.com/fQwQf/a20-tests/blob/main/tools/tests/cli_application_guest.py)；应用总状态见[应用契约表](application-contracts.md)。这不证明公网路由器 ICMP、IPv4-mapped IPv6、同步本地错误进入扩展队列、所有 IP 扩展头或其它后端的完整 Linux 兼容。syscall 覆盖表保留 partial。
