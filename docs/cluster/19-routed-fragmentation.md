# 多跳 UDP 分片

## 1. 设计

CL 的逻辑消息与物理帧分别限额。MCU 保留静态单帧预算，完整主机支持有界重组；
网关转发物理帧，既不重组业务，也不改写来源 ID、epoch 或事务号。
[14-fragmentation.md](14-fragmentation.md) 的 v1 格式与固定 UDP 分片几何保持不变。

CS/1 挑战同时确认最终身份、启动 epoch、最小物理 MTU 和端到端分片资格：

1. 主机仅在本地 UDP 下一跳已完成 HELLO、支持 FRAGMENT、MTU=1472 时发布该位。
2. 每个中继检查入站、出站邻接能力；任一不是支持分片的 UDP 路径，就清除此位。
   中继同时下调 MTU，重新计算 CRC；后续中继不能恢复被清除的能力位。
3. REPLY 保留 PROBE 已取得的路径限制，再与本机及回程下一跳能力取交集。
4. 来源只接受当前 cookie 与自身 epoch 匹配的回复，并再次检查本地下一跳能力。
   双方分别完成反向挑战，才接纳带最终 epoch 对的业务分片。

最终物理 MTU=1472 且 FRAGMENT 有效时，逻辑上限取双方资源档的较小值：默认
4096 B，服务器 65590 B（最多 65536 B 业务回复）。经过 UART、MCU、旧协议
邻接或不能分片的节点时回退到最小物理 MTU；超限在本地结束调用，不静默等待。

中继本身的默认档不限制两端服务器的逻辑 64 KiB 消息，因为它只转发最多
1472 B 的帧。目的端先校验当前端到端会话与双方逻辑限额，再分配重组资源。
重组仍受槽位、总字节、64 分片、30 s 首片年龄与冲突毒化限制；同一事务重传
保持 txid 不变，只有完整重组后的请求才能进入服务接纳和去重。
首片期限及完成/退休后的提前过滤见 [14-fragmentation.md](14-fragmentation.md)。

## 2. 兼容与范围

不增加 wire 版本、syscall 编号或 Native 能力位；FRAGMENT 仍是 wire 能力。
旧 CS 实现始终清除 FRAGMENT，因此新旧混合路径继续单帧；原有 v0 金样不变。
路由、邻接 epoch 或分片资格变化会使原 CS 路径失效，旧代理不能继续使用旧资格。

本实现不把大消息拆成 256 B UART 帧，不在中继合并后重新分片，也不支持不同
UDP MTU 的动态分片几何。要增加这些能力，需扩展分片计数/编码和 MCU 资源预算；
不能仅把现有分片发给单帧 leaf。CRC 与地址绑定仍沿用受控网络信任边界。

## 3. 验证方法

```sh
python3 tools/check-cluster-udp.py --routed --faults \
  --log-dir .kernel-build/cluster/routed-default
python3 tools/check-cluster-udp.py --routed --faults --profile 3 \
  --log-dir .kernel-build/cluster/routed-server
python3 tools/check-cluster-udp.py --routed --faults --profile 3 --peer-profile 2 \
  --log-dir .kernel-build/cluster/routed-mixed
```

测试使用三套独立 RISC-V QEMU 内核。端点各有一个网关种子和经网关到对端的
路由；中继仅设置身份及两条直连路由。两个端点分别执行可靠调用与分片 echo。
真实 Ethernet 字节流注入丢片、重复、乱序、丢请求/回复/ACK/收据以及全丢请求
直到重试耗尽；不合成业务回复。中继输出的每个分片要求 TTL=7、长度≤1472，
源端初始 TTL=8。服务实际执行次数和重复计数由目标内核返回。

日志、源文件哈希、配置档、实际注入次数与线帧轨迹保存到指定目录。
运行证据见 [../status/current/cluster-implementation-status.md](../status/current/cluster-implementation-status.md)；本节列出
方法，不把尚未通过的命令视为验收结果。UDP→UART 网关的单帧限制回归另见
[15-routed-sessions.md](15-routed-sessions.md) 与 [18-wordcount.md](18-wordcount.md)。

默认、服务器、服务器/默认混合档三种命令现已全部通过；服务器档两端回显
65534、65535、65536 B，混合档最高 4042 B。每个配置两端大消息前后网络堆
均为 736 B，结束重组上下文/字节均为零。中继转发的实际请求和回复分片都
记录在 `udp-faults.json`；同一事务重复重组没有增加服务重复执行计数。
