# 统一工作协议与 agent

MCU、内核测试服务和 Native 用户态服务共用
`kernel/include/uapi/a20/cluster_work.h`，不复制算子或消息实现。
业务格式独立于 CL、UDP、UART 和本地 channel；消息仍由 CL 事务负责重放。

## 1. 请求与结果

所有整数显式小端，头部固定 32 字节，不发送 C 结构体内存镜像。

| 偏移 | 长度 | 请求 | 结果 |
|---|---|---|---|
| 0 | 2 | ASCII `CW` | ASCII `CW` |
| 2 | 1 | 版本 1 | 版本 1 |
| 3 | 1 | 种类 | 回显种类 |
| 4 | 8 | job_id | 回显 job_id |
| 12 | 4 | shard_id | 回显 shard_id |
| 16 | 4 | operator_id / 进程 manifest ID | 回显 ID |
| 20 | 4 | 本次执行 budget_ms | 工作状态 |
| 24 | 4 | 输入长度 | 输出长度 |
| 28 | 4 | attempt | 回显 attempt |

头后紧接输入或输出，长度必须精确匹配。结果解码器同时校验作业、分片、
算子、种类和 attempt，旧尝试的迟到回复不能完成新尝试。
任务种类为 QUERY=1、OPERATOR=2、PROCESS=3。
非查询请求必须有非零 job_id 和时间预算；预算超出 agent 上限会拒绝。
工作状态为 OK、INVALID、UNSUPPORTED、NO_SPACE、DEADLINE、CANCELED、FAILED。
工作失败以完整结果返回，不关闭业务连接；CL 传输错误仍按代理取消规则处理。

QUERY 要求无输入、operator_id=0，返回 24 字节能力头：
kind 位图、最大输入字节、最大输出字节、最大在途、最大预算毫秒、算子数，
随后每个算子用 16 字节发布 ID、flags、最大输入、最大输出。
REPLAY_SAFE 表示作业层可重派；ASSOCIATIVE 表示可做树形归约。
进程能力只在实际装载 manifest 时发布，当前 manifest ID 固定为 1。
算子输出上限不包含 32 字节工作头，能力查询自身不受算子输出上限限制。

## 2. 共同算子

- DOT=1：输入 `u32 n; i32 a[n]; i32 b[n]`，精确长度 `4+8n`；输出 `u64 sum`。
- SUM64=2：输入连续小端 u64，长度必须为 8 的倍数；输出 `u64 sum`。

累加都按模 2^64 定义，DOT 的单个有符号乘积在 i64 内计算。
这使 32 位 MCU 与 64 位服务器在溢出时仍有一致语义。
旧 dot 槽位保留 `u32 status + i64 sum` 格式，但已改为调用同一 DOT 实现。

## 3. 静态与托管资源

MCU 默认发布 dot 槽位 1、work 槽位 2。工作执行无堆分配，最大输入 140 B、
算子输出 168 B、单在途、256 B 完整回复缓存。调用方先查询能力再切分输入。
板级代码可在握手前通过 `a20_mcu_leaf_set_agent()` 安装静态生命期的回调表。
重复 ID、超出静态缓冲、进程回调和不符合单在途预算的注册会拒绝。
`a20_mcu_leaf_configure()` 接受唯一持久身份与非重复 boot epoch；默认演示值
仍只用于测试；STM32 main 已接入 UID 与持久 epoch，物理验证尚未完成，见 12 文档。

Native 程序 `cluster-agent-<arch>` 导出 `work` 服务，默认输入/输出预算各 1024 B。
它有界轮询一个远程 FIFO 与最多 8 个本地连接；本地 connect 的独立 channel
不会和远程回复 worker 竞争。FIFO 回复投递失败会终止服务，防止后续结果错配。
Native 的空句柄是 `0xffffffff`，初始化必须显式设置，不能用零代替。

```sh
/bin/cluster-agent-rv 101112131415161718191a1b1c1d1e1f
# 可选：为 PROCESS 的 manifest ID 1 预先装载一个可执行文件
/bin/cluster-agent-rv 101112131415161718191a1b1c1d1e1f /bin/cluster-task-probe-rv
```

如果身份已设置，agent 读取快照并验证配置一致；重启不重新设置身份。
当前进程请求输入为空，输出 `u32 exit_code; u32 exit_reason`，环境和文件句柄
不从远端任意传入。路径由启动配置提供，不从网络请求解释。
进程超时终止子进程，再用最多 1 s 的独立清理预算回收退出状态。
Native SIGKILL 已修正为作用于被引用的目标任务，不能结束发起取消的 agent。

算子回调需合作遵守预算/取消；通用 C 引擎在回调前后检查时钟和取消函数，
不能抢占一个不返回的裸机回调。板级 watchdog 和可抢占算子属于继续硬化范围。
CL 缓存保证当前会话内的事务去重，不持久化作业结果。
跨会话重派仍须由作业调度器按 `(job_id, shard_id)` 去重并检查 REPLAY_SAFE。

## 4. 发现与恢复

`cluster_snapshot`（Native 0x0526）只读查询 SELF、ROUTE、EXPORT。
SELF 提供持久身份、当前 boot epoch 与资源档上限。
枚举由 index=0、generation=0 开始，后续使用 next_index 和返回的 generation。
BUSY 表示表发生变化，必须从头重读；NOT_FOUND 表示结束。
每张表的 generation 独立，不能把路由表 generation 用于服务表。
没有整个多表原子快照的承诺。

远端使用固定发现槽位 `0xfffffffe`，该端点不依赖用户态守护进程存活。
请求为 16 B：`CD`、版本 1、保留 0、u32 index、u64 generation。
结果固定 128 B：状态、generation、next_index、node_id、boot_epoch、caps、
profile、slot、flags、name_len 与 64 B 服务名。最后 4 B 保留为零。
FOUND、END、CHANGED、INVALID 分别表示条目、结束、重新枚举、请求非法。
完整偏移以公共 `cluster_directory.h` 为准。
内核不发布 LOCAL_ONLY 服务；叶子发布 dot/work。
发现不是密码学认证，沿用 CL 的受控网络信任边界。

## 5. 验证

`make check-cluster-unit` 执行共用协议、10 万个畸形工作请求、能力/资源拒绝、
取消与期限检查。实际 UART 主/叶配对测试使用同一客户端工作请求，和主机执行器
逐字节比较 DOT 与 PROCESS 拒绝结果，并发现叶子的 work 服务。

`make check-cluster-reliable` 在两个独立内核之间调用 work、查询目录，并继续
测试丢包/重放/重启。`make smoke-native-cluster` 启动真正的用户态 agent，调用
QUERY、DOT 和子进程任务，核对退出码 42；随后重启 agent，执行故意不退出的
子进程，验证 DEADLINE 后 agent 继续响应，boot epoch 保持不变、导出槽位更新。
没有物理板运行证据；不能把 host harness 或固件构建写成物理 MCU 验收。
