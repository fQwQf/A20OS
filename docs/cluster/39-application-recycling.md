# 持续执行应用与安全回收

## 用法

新 `appctl-run` 使用可回收的 CA/2 接口。命令观察到真实 EXITED 或 FAILED 后，
提交 RELEASE 回收记录，再返回已经观察到的退出码。8 项表容量因此限制尚未
回收的并行记录，而不再限制正常命令累计执行次数。接收不到回收确认时只在
原 owner 重试，不改变程序退出码；仍无法确认时打印明确提示，保留结果未知。

Native SDK 显式使用：

```c
a20_app_session_t session;
a20_app_ticket_t ticket;
a20_app_result_t offer, result;
if (a20_app_connect_reusable(domain, 8000, &session) == 0 &&
    a20_app_offer(&session, manifest, 4000, &offer) == 0 && offer.token &&
    a20_app_start_offered(&session, manifest, nonce, offer.token,
                           4000, &ticket, &result) == 0) {
    /* 保存 ticket；使用 query 等待真正 EXITED 或 FAILED，再提交： */
    a20_app_release(&session, &ticket, 4000, &result);
}
```

跨候选节点仍按实际活动程序数选择：

```c
a20_app_start_any_reusable(domains, count, manifest, nonce,
                            30000, &ticket, &result);
```

两种启动均在传输前写回 ticket。所有重试和结果查询都使用原 ticket；重新获取
offer/token 表示一次新执行，不能用来处理原启动结果未知。resume/query/stop
自动按 ticket 中的版本使用同一协议。运行中、停止中或尚不可可靠观察但仍持有
任务引用的 LOST 记录，RELEASE 返回 BUSY/WOULD_BLOCK，不停止、不释放该任务。

原 connect/start/start_any 入口和已有二进制继续使用 CA/1，保留原来的累计
8 项终态保留语义；不会隐式淘汰其记录。新可回收接口和新版 CLI 需要 CA/2
owner。CA/1 服务名保持 `instance.apps`，CA/2 使用新增 `instance.apps2`。
新版 appd 为两个入口使用独立通道/FIFO，并在同一个线程串行调用同一 owner；
两者共享真实任务负载和容量表。新 SDK 先查找 CA/2 服务名，不会把新版本
请求发送给只支持 CA/1 的旧服务，避免未知协议帧关闭旧服务的请求队列。

## 不重复执行的机制

每个槽位带一个 owner 分配的非零 64 位 token。HELLO 返回一个当前空槽的
编号快照，不预留该槽；多个调用者可能观察到同一个 token，只有第一个成功
START 的请求能够占用它。已占用槽只接受该协议版本、client、nonce、manifest
匹配的操作，其他请求冲突。CA/2 执行键包含实际 controller ID、token、client ID
和 nonce；同一 token 的重复 START 返回原根任务和结果。

RELEASE 只回收终态或尚未接受 START 的空槽，然后分配一个从未使用过的
新 token。旧 token 不再存在，任何迟到的 START/QUERY/STOP/RELEASE 都返回
STALE/EXPIRED。回收空槽还能先于尚未到达的首次 START 取消该执行键。
竞争者已占用的槽不能借另一个 client/nonce 回收。CA/1 记录也不能通过 CA/2
别名回收。task_wait 观察退出时只释放一次真实任务引用；RELEASE 仅回收记录，
不再次释放引用。

编号只在当前真实 controller 生命周期内单调增长，达到 UINT64_MAX 后不回绕；
耗尽的空槽不再向 CA/2 提供 token。适配器不得在同一 controller 身份下重新
初始化 owner/编号序列；服务重启必须取得新的真实控制任务对象 ID。token 是
执行协议的槽位版本，不替代或修改全局任务名，也不把槽编号伪装成任务身份。
固定表和一个计数器即可持续复用，无需无限保存旧 nonce 或暂停其他运行程序。

回收不是持久化。断电/控制进程重启仍按已有真实 controller/boot_epoch 失效
规则处理。没有自动清理无人确认的终态，也不自动回收仍存活或 LOST 的任务；
无人持有 ticket 的记录可能继续占用容量，后续仍需显式托管/恢复策略。
跨重启 exactly-once 与自动迁移已由
[62-release-scope.md](62-release-scope.md) 明确排除出 1.0；这里的持续复用
只覆盖当前 controller 生命周期。

## CA/2 线格式

请求仍为 176 B、回复仍为 192 B，可靠 UART 总长仍为 230/246 B。基础布局见
[37](37-application-execution.md)。变更如下：

| 字段 | CA/2 含义 |
|---|---|
| 请求/回复偏移 2 | version=2 |
| op | HELLO=1、START=2、QUERY=3、STOP=4、RELEASE=5 |
| 请求 24–31 | HELLO 为零；其他操作为非零 slot token |
| 成功 HELLO 回复 16–23 | 当前空槽 token；无可用 token 时为零 |
| 其他回复 16–23 | 原样 nonce |
| RELEASE 成功回复 | state/exit/root 为零；容量与实际 occupied/active 为回收后的值 |

其他 cookie、controller/client/manifest、负载条件、字节序、状态码和保留位
规则保持不变。CA/1 的请求 24–31 仍必须为零，不接受 RELEASE。解码失败不
发布部分输出。owner 的派发、轮询和回收必须串行；适配器负责这一调用约束。

## 实际验收要求

```sh
make check-cluster-apps
python3 tools/check-object-directory-portability.py --kind apps
make check-cluster-unit
make CCACHE= PYTHON=<Unicorn/pyelftools Python> check-mcu-program-rtl
```

三台真实 QEMU 内核分别运行 RV64、AArch64、x86 ELF。每个 owner 持续保留一个
运行程序，再用另一个槽依次启动、观察退出并回收 16 个真实程序。校验原任务
身份不变，已回收的旧 START 失效，取消尚未接受的 token 不执行程序。回收
回复被主动丢弃时，原 ticket 查询及重试 START 仍失效。CA/2 START 回复
另外主动丢弃，要求用原 ticket 找到同一个运行根任务并重复 START，不重复执行。原 CA/1 的六程序
2/2/2 分配、重复请求和重启验收继续执行。

同一个 appd 上另外连续运行超过 8 次真实 CLI，验证退出码、参数/环境及槽位
自动回收，不用重启控制进程。公共头还经过固定两槽 32 次复用、竞争 client、
计数器耗尽、CA/1 隔离、跨 ISA 执行及 ASan/UBSan 验证。

MCU 测试配置通过同一 CA/2 owner 实际装载并运行 ARM ELF/SVC，在真实任务
退出后回收记录，验证旧 START 失效、槽可重新提供且任务引用只释放一次，
继续检查共享堆全部回收。它仍是指令执行/双端口 RAM RTL 证据，尚不是物理板
或 MCU 常规网络 appd 服务的证据。

## 当前证据与未解决故障

2026-10-13 的三架构真实内核运行已完成上述 48 个退出/回收周期、12 个 CLI
退出码 37、1 个远程停止退出码 137，以及丢失 CA/2 START 和 RELEASE 回复
后的原 ticket 对账。RV64 发起、AArch64 和 x86 参与的原构建重跑通过；另一次
带运行队列诊断的完整重跑也通过。交换为 AArch64 发起、RV64/x86 参与后，
相同完整流程通过，证据在 `apps-recycle-arm-controller/`。上版 `c0421183` 的三个真实客户端二进制
接入新版双入口 appd，同样通过原 CA/1 全流程，未重新编译这些客户端。

首次双入口运行曾在 AArch64 出现 `proc_put: pid=1 reference underflow old=0`，
调用链包括 `proc_runq_pick_local`。后续完整重跑和 init 引用观察点未复现；
目前没有确定根因或内核修复，不能据后续通过就宣称该故障已经解决。失败日志
保留在 `.kernel-build/cluster/apps-recycle-services/`，通过的原构建运行在
`apps-recycle-watch/`，旧客户端验收在 `apps-recycle-legacy/`。此问题仍是集群
长期稳定性验收的待办；检查器现在在任何节点出现内核 panic 时立即失败并
保留错误，而不只等到网络超时。

本轮默认 RV64、AArch64、x86 内核与改动前哈希一致，回收代码位于显式启动的
应用服务和公共 owner；普通启动未增加服务或调度扫描。MCU ELF/SVC + 双端口
RAM RTL 验证通过（seed 22），最终构建与该已测 ELF 字节相同。格式检查仍被
原有 114 个文件阻断，本轮没有扩大格式基线。上述证据没有覆盖物理 MCU、
常规 MCU 网络应用服务或真实 Minecraft Java 客户端。
