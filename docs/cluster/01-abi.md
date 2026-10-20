# Native 集群 ABI 使用手册

七个 syscall 已接入真实内核数据面，号段 `0x0520`–`0x0526`。
结构体的权威定义为 `kernel/include/abi/native/types.h`，用户包装在
`user/liba20rt/a20_cluster.h`。协议版本与 Native 参数结构体 version 是两套
不同版本：参数头当前填 `size=sizeof(args), version=1`。

## 1. 通用规则

参数通过单个结构体指针传入，返回非负成功值或负 A20 错误码。
node_id 为 16 字节持久身份；全零代表 LOCAL，全 ff 为保留广播值，均不能
用作本机或普通远程节点身份。哈希为 FNV-1a32，仅作索引，路由安装拒绝冲突。

身份配置、导出和路由修改要求集群管理权限，连接、事件订阅和状态查询不要求
管理权限。代理句柄只允许传数据，不能跨机器传递本机 handle。

## 2. 七个调用

| syscall | 输入要点 | 成功结果 |
|---|---|---|
| `cluster_set_self` | node_id、caps | 0；只允许成功一次 |
| `cluster_export` | channel、service_name、name_len、flags | 新 slot |
| `cluster_connect` | node_id、slot 或本地服务名、flags、timeout_ms | 普通 channel 句柄 |
| `cluster_route` | ADD/DEL/REPLACE/DEL_MATCH/LEARN、node_id、transport_id、next_hop、metric | 0 |
| `cluster_event_subscribe` | 事件 mask | EventQ 句柄 |
| `cluster_link_status` | node_id；LOCAL 代表汇总 | 状态/计数写回参数 |
| `cluster_snapshot` (0x0526) | kind/index/generation | 当前身份、资源档、运行资源计数或单条路由/服务快照 |

`set_self` caps 使用 RELAY、RELIABLE、LEAF，以及显式开启的 ONEWAY 位；
LEAF 与 RELAY 互斥。单向接收接口、能力升级要求与接纳语义见
[22-oneway.md](22-oneway.md)。
RELIABLE 为数据面实际能力，内核发布时附带它，是否能建立可靠连接还取决于
对端版本与能力。角色不由处理器型号或资源档固定。

## 3. 导出与引用所有权

```c
a20_channel_pair_t pair;
a20_channel_create(&pair);
uint32_t slot;
a20_cluster_export(pair.endpoints[0], "echo", 0, &slot);
/* 成功后不再从配对端点读取，避免与内核回复线程竞争。 */
a20_hdl_close(pair.endpoints[1]);
/* 服务在 pair.endpoints[0] 接收请求并按顺序回复。 */
```

登记增加核心自己的 peer 引用，不消费另一个用户 handle 的引用。导出成功后
应用应关闭自己不再使用的配对端点。服务端点关闭时核心撤销导出并取消未回复
事务。导出失败不消费现有用户句柄。

名称长度 1–64。同名登记没有 REPLACE 时返回 EXISTS；REPLACE 关闭旧导出并
分配新 slot。slot 单调且不复用，编号耗尽拒绝，旧 slot 不会意外指向新服务。
LOCAL_ONLY 禁止远程访问。WORK（bit 2）声明该服务支持 CW/1，供能力目录识别；
未设置时 clusterd 不向服务发送 CW 查询。服务须按接收顺序回复；当前无乱序业务完成接口。
ONEWAY（bit 3）导出仅接收业务 SEND，不与 WORK 组合。连接须设置 RELIABLE
与 ONEWAY，返回只写 channel handle；普通 CALL 连接与单向导出不能混用。

本地 connect 使用独立 channel pair，把连接端点作为 handle 交给服务。
支持本地连接的服务须接收此握手并服务新端点，不能只把零长度消息当业务请求。

## 4. 路由与传输

| transport_id | next_hop |
|---:|---|
| 0 loopback | 4 B 虚拟节点号 |
| 1 UDP | IPv4 4 B + 网络序端口 2 B |
| 2 UART | 小端短地址 2 B |

例如 `10.0.3.3:44020` 为 `{10,0,3,3,0xab,0xf4}`。
ADD 已存在返回 EXISTS，REPLACE 为 upsert，DEL 不存在返回 NOT_FOUND。
配置竞争可返回 BUSY；无效参数、表满、哈希冲突或适配器启动失败不应留下
不可用的新路由。删除/更换下一跳会取消该节点旧代理。

loopback 虚拟节点共享本机导出表，仅用于测试。UDP 路由启动非阻塞 socket
和握手；UART 还需板级注册字节驱动，不能仅安装路由就假定物理串口已接线。

## 5. 连接与失败

```c
a20_handle_t remote;
a20_status_t st = a20_cluster_connect(&peer, slot, NULL,
                                      A20_CONNECT_RELIABLE, 0, &remote);
```

远程按 slot 连接要求已配置路由。可靠连接要求已握手的对端支持 wire v1 和
RELIABLE；版本 0 peer 仍可用普通连接。远程按服务名与 `service://any/name`
需要后续控制面解析，当前不能把未实现的名称服务当作内核功能。

成功返回后调用方使用普通 channel send/recv/call。服务端关闭、链路 DOWN、
队列或在途耗尽、超 MTU、错误回复、期限或重试耗尽都通过 peer-closed 唤醒，
现有 syscall 表现为 `-A20_ERR_CANCELED`。精确远程错误保留在协议层，当前
没有逐调用的错误查询接口。发送成功只说明请求进入本机队列，不保证远程执行。

当前远程在途期限固定 30 s；可靠调用另有最多 8 次重试。
`timeout_ms` 尚未接入完整建链/调用期限控制，非零值不能视作已兑现的期限保证。
每次连接失败、节点重启或断链后应重新解析服务并重建代理，不能复活旧句柄。

## 6. 事件与状态

事件 mask 选择 LINK_UP、LINK_DOWN、ROUTE_LOST、EXPORT_DROPPED。
通过 EventQ 的 `a20_event_notify` 投递，事件类型为 `32 + kind`，data0 为节点
哈希，data1 编码传输/原因。订阅仍须处理队列溢出并重新取得状态。

link status 包含 state、rtt_us、帧数、丢弃数、retransmits 和 HELLO 年龄。
loopback 常 UP；未握手的真实链路不能因发送成功显示 UP。
UDP SUSPECT/RTT 细节和重启后完整状态重建接口仍待扩展；控制面不能只依赖
可能丢失的增量事件来重建全量目录。

## 7. 资源档与验证

`CLUSTER_PROFILE=1/2/3` 分别为 MCU、DEFAULT、SERVER。MCU 构建强制档 1，
完整主机默认档 2。资源预算见 [03-kernel-impl.md](03-kernel-impl.md)。
NOMMU leaf 直接运行相同线协议与算子，不要求具有用户态 Native syscall。

```bash
make check-cluster-unit
make check-cluster-loopback
make smoke-native-cluster
python3 tools/check-cluster-udp.py --faults --restart
```

验收状态、物理硬件边界和剩余功能见 [../status/current/cluster-implementation-status.md](../status/current/cluster-implementation-status.md)。

`cluster_snapshot` 的只读枚举与恢复契约见 [09-work-agent.md](09-work-agent.md)。
kind=3/4/5 分别提供运行资源、重组资源与聚合链路计数，仍使用同一 192 B
信封；链路统计包含此前只保存在内核中的 malformed 计数。指标与诊断边界见
[20-hardening.md](20-hardening.md)。

## cluster_connect v2：绑定解析得到的启动会话

v1 参数布局和 reserved=0 要求保持不变。v2 使用
`a20_cluster_connect_args_v2_t`：完整 v1 base 后追加 u64 expected_boot_epoch；
base.size 必须覆盖完整 v2，base.version=2，RELIABLE 必须设置，epoch 必须非零。
比 v2 短的参数、零 epoch 或没有 RELIABLE 均返回 INVALID_ARGUMENT。

LOCAL 连接核对本机 boot epoch；远端在创建代理前核对协商到的目的会话。
不一致返回 NODE_UNREACHABLE，不能把目录解析到的旧服务派发给重启后的节点。
已创建代理仍绑定这个会话，后续重启按既有链路取消规则处理。
SDK 入口 `a20_cluster_connect_bound`；service URI 和作业驱动使用它。


### 动态路由操作扩展

`cluster_route` 原结构和 syscall 号不变，新增 op=3 `DEL_MATCH` 与 op=4
`LEARN`。DEL_MATCH 仅在当前 node、transport、next_hop、长度和 metric 全部
相等时原子删除；不匹配返回 BUSY，没有目标返回 NOT_FOUND。LEARN 只新增或
替换带 `A20_ROUTE_DYNAMIC=0x80000000` 标记的路由，拒绝覆盖显式路由（EXISTS），
且传入 metric 必须含标记。低 31 位为传播距离。管理员权限要求不变。
显式 REPLACE 可覆盖动态路由；旧守护进程随后清理时不能误删用户的新配置。
