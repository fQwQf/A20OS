# 可配置的远程页服务：SSI 的第一阶段

本阶段增加独立机器之间的远程页服务 `cluster.memory`、Native SDK、配置文件和 Rust 事务所有权核心。每个节点仍运行自己的内核，通信走既有 CL 可靠通道；不需要共享物理 RAM。该服务显式启用，与现有作业、应用启动和共享 RAM SMP 独立。

**当前接口提供私有页快照和原子整页提交，不提供透明分布式 mmap。**读取的本地数组不会自动刷新，对它的普通写入不会自动传到远端。普通 pthread 尚不会因此跨机器执行。这些限制适用于所有模式，不能把本服务描述为已经完成单一系统映像或缓存一致的分布式共享内存。

## 1. 选择运行方式

| 需求 | 配置 | 当前行为 |
|---|---|---|
| 禁止远程内存共享，减少通信 | 不启动服务，或 `mode off`；客户端 permission=OFF | 服务不导出端点；客户端在发现或网络调用之前返回 NOT_SUPPORTED。已有服务/作业通信不受影响 |
| 分发只读数据，应用自行控制刷新 | `mode read-only`，可选 `initial-file` | 允许显式读取页快照，拒绝写事务；调用者可保留快照以减少通信 |
| 多机交换可修改的数据页 | `mode read-write` | 允许版本化快照和 BEGIN/STAGE/COMMIT/ABORT；整页提交原子可见 |
| 限制服务静态内存容量 | 构建时 `CLUSTER_MEMORY_PAGES` | 同时缩小 Rust 页数组及 C owner storage 上界 |
| 限制可用页与数据流量 | `max-pages`、`bytes-per-second` | 限制对外容量、读出和暂存写入的数据字节速率 |

这里的“只读/可写”是远程页服务的访问策略，不是 POSIX MAP_SHARED 的语义。在资源紧张时可显式把数据放在远端，通过 SDK 取回当前需要的页；尚不能自动把普通进程的堆换出到其他机器，也不保证本机内存占用因此自动下降。

## 2. 构建

```sh
# 默认 CLUSTER_MEMORY=0：普通构建不依赖或启动此服务。
make ARCH=x86_64 ABI=both CLUSTER_MEMORY=1 CLUSTER_MEMORY_PAGES=8 native-cluster-memory

# 小容量版本：一个 4096 B 远端页。
make ARCH=x86_64 ABI=both CLUSTER_MEMORY=1 CLUSTER_MEMORY_PAGES=1 native-cluster-memory

# 生成包含服务和测试程序的系统镜像。
make ARCH=x86_64 ABI=both CLUSTER_MEMORY=1 CLUSTER_MEMORY_PAGES=8 dev-build

# 查看本次配置的两个实际输出路径。
make -s ARCH=x86_64 CLUSTER_MEMORY=1 print-cluster-memory-paths
```

初始支持 x86_64、riscv64、aarch64、loongarch64 的 Native 用户程序构建；安装与目标对应的 Rust 标准 core target。该 Rust 库运行在用户态，不能复用内核的专用软浮点/代码模型参数。其他 ISA 暂不提供此服务构建目标。 Rust 目标分别为 `x86_64-unknown-none`、`riscv64gc-unknown-none-elf`、`aarch64-unknown-none`、`loongarch64-unknown-none`。

`CLUSTER_MEMORY_PAGES` 接受 1/2/4/8/16/32/64，默认 8。owner storage 的 C 预留上界为 `pages * (4096 + 32) + 8192` B；另有初始文件缓冲、配置、代码、主线程栈和通道资源。Rust 初始化在预留空间内完成，不在栈上再构造完整页表。运行时 `max-pages` 只限制使用量，不缩小已编译的静态数组；降低内存占用需要修改构建容量并重新部署。SDK 整页读取另外使用约 4 KiB 临时快照栈空间。

构建开关控制新增服务的构建依赖，不自动清除用户输出目录中旧版本的可执行文件。要求镜像完全不携带它时，使用干净的用户输出目录重建。运行时 `off`会正常退出，不创建导出、网络会话或周期泵；可执行映像本身仍有装载开销。

## 3. 配置、部署与启动

先按 [17-deployment-supervision.md](17-deployment-supervision.md) 配置节点身份、 UDP 种子与公共 runtime 实例。参与节点须绑定同一 instance UUID，各自使用不同 domain/node ID；boot epoch 必须符合该文档的启动代次要求。服务自身不设置或覆盖身份、不安装路由。

将以下内容保存为目标节点上的 `/bin/memory.conf`（路径可自行指定）：

```text
version 1
mode read-write
max-pages 8
transaction-ms 5000
bytes-per-second 65536
```

在目标 A20OS 中启动：

```sh
/bin/cluster-memory-x86_64 /bin/memory.conf
# RISC-V 对应名称：/bin/cluster-memory-rv
```

返回 `CLUSTER_MEMORY: ready` 表示端点已导出。守护进程在前台运行，可由启动脚本或外层服务管理器监管；当前三职责的 cluster-supervisor 配置没有新增`service memory` 字段，不要把它加入旧配置。SIGTERM 撤销服务并退出；异常退出也由已有内核导出生命周期关闭端点。

| 字段 | 默认/范围 | 精确语义 |
|---|---|---|
| `version` | 必填，必须 1 | 配置版本 |
| `mode` | 默认 `off`；off/read-only/read-write | 只有显式允许时才导出服务；其他参数不会隐式启用 |
| `max-pages` | 默认编译容量；1..编译容量 | 可访问页索引为 0..pages-1，每页 4096 B |
| `transaction-ms` | 默认 5000；1..30000 | 从 BEGIN 被接受起的固定期限；STAGE/重试不续期 |
| `bytes-per-second` | 默认 0；0 或 512..4294967295 | 0 不限速；其他值为整个服务所有客户端合计的数据字节/秒，允许一秒额度的突发 |
| `initial-file` | 可选，绝对路径，最多 127 个 ASCII 非空白字符 | 导出前装载初值；文件最多 pages×4096 B，末页补零，未覆盖页为零 |

配置拒绝未知/重复字段、额外参数、非法数值、NUL 和超过 4096 B 的文件；解析失败不发布部分配置，不导出服务。不支持热重载，修改后需重启服务。初始文件读取失败或超容量同样不导出服务。`off` 不打开初始文件。

只读数据示例：

```text
version 1
mode read-only
max-pages 2
initial-file /bin/model.bin
bytes-per-second 32768
```

初始文件只用于启动时的初值，之后的写事务不写回该文件。重启会丢弃内存中的提交结果，并从初始文件重新装载；未指定初始文件则重新从全零、版本 1 开始。

读/写传输分成 512 B 数据块。限速统计 READ 回复正文和 STAGE 请求正文，不含协议头、握手、重传等网络开销，因而不是网卡总线速上限。额度不足返回 THROTTLED，映射为 WOULD_BLOCK；SDK 不忙循环重试。低额度下应延长事务期限，或由调用者分批暂存并适当等待。小于 512 的非零速率会被拒绝，避免整块请求永远无法取得一秒突发额度。

服务适用于有足够业务 MTU 的 UDP 路径：消息最大 704 B，不支持目前 256 B UART leaf 路径。链路不能承载时调用失败，不会静默降级成不完整数据。

## 4. 客户端 API

包含 `user/liba20rt/a20_cluster_memory.h`。客户端明确选择 owner 的 domain 和本次会话的 OFF/READ_ONLY/READ_WRITE 权限；READ_ONLY 会在本地拒绝写入。客户端限制与服务端策略取交集，不能据客户端配置提升服务端权限。

```c
a20_memory_session_t s = {.endpoint = A20_HANDLE_NULL};
a20_memory_ticket_t ticket;
uint8_t data[A20_MEMORY_PAGE];
uint64_t version, committed;

/* owner_domain 是已配置的远端节点 UUID；不是进程本地指针。 */
int status = a20_memory_connect(owner_domain, A20_MEMORY_READ_WRITE, 5000, &s);
if (status == 0)
    status = a20_memory_read_page(&s, 0, data, &version, 5000);
if (status == 0) {
    data[0] = 42;
    status = a20_memory_begin(&s, 0, version, unique_nonce, &ticket, 1000);
    if (status == 0)
        status = a20_memory_stage_page(&s, &ticket, data, 3000);
    if (status == 0)
        status = a20_memory_finish(&s, &ticket, A20_MEMORY_COMMIT, &committed, 1000);
    /* 已知未提交且仍持有有效连接时，可显式 ABORT；否则由固定期限丢弃暂存。 */
}
a20_memory_close(&s);
```

每个 API 的 budget 是整个该次操作的预算，不是每个数据块重新计时。 BEGIN 的 nonce 由调用者为每个新事务分配。一个会话只能由一个调用线程使用；多个线程请分别连接，会话字段和通道回复不能无锁并用。

- `read_page` 返回单一版本的私有快照，只有完整读取成功才改写调用者的数组和 version。并发提交使中途版本变化时返回 EXISTS，输出保持不变；调用者可自行决定重新读取。没有自动刷新或后台轮询。
- `begin` 要求调用者已经观察的非零版本仍为当前版本。一个服务只有一个暂存写者，可跨页竞争；其他写者返回 WOULD_BLOCK。先把所有页写者串行化，避免初版为了并行暂存额外占用多份 4 KiB 内存。
- `stage_page` 暂存完整页的八个块，普通读取仍看到原提交版本。相同块的重发必须逐字节一致；同 token 改写已确认块返回 EXISTS。
- `finish(COMMIT)` 只在八块齐全且事务有效时一次性发布，版本加一。`finish(ABORT)` 仅丢弃暂存，已返回给调用者的私有数组无需撤销。
- 超时或连接错误后 SDK 关闭 endpoint，防止迟到回复被下一调用消费。**COMMIT 超时表示结果未知**，不能换 owner 或用新 nonce 自动重写。同一任务可重新连接原 owner，以保存的 ticket 显式重试原 COMMIT。
- 服务保留最后一次提交的完成记录，允许其 COMMIT 重放返回原版本而不再写入。后续提交会替换该记录；旧完成记录被替换后返回 EXPIRED，不推断其未执行。 BEGIN、ABORT 和过期事务不提供持久历史查询。ticket 不是长期提交日志。
- 服务进程身份是票据的一部分；进程或内核重启后旧票据失效，不能指向新服务的同号 token。数据和票据都没有跨重启恢复保证。

主要错误：NOT_SUPPORTED 为关闭；ACCESS 为权限不足；EXISTS 为版本或块内容冲突；WOULD_BLOCK 为占用、限流或提交不完整；EXPIRED 为旧 owner/过期 token； NO_SPACE 为版本/token/期限溢出拒绝。底层传输错误保持原错误码。

## 5. 并发、安全与 Rust/C 分工

Rust `cluster_memory_state.rs` 拥有提交数据、版本、写者 token、分块接纳位图、期限、最后完成记录和全局额度。所有可变状态位于 UnsafeCell 内，唯一入口通过 AtomicBool acquire/release 非阻塞门保护；RAII guard 确保所有返回路径释放门。争用返回 BUSY，不在用户服务里自旋持锁或跨网络等待。

C 负责配置、文件初值、Native 身份/通道、服务生命周期与 SDK。FFI 初始化只在导出前进行，存储至少 8 字节对齐，销毁前必须停止所有调用。RPC 缓冲可以互相重叠，但不能覆盖 owner storage。Rust 不会自动证明 C 侧的生命周期正确。

目前同实例内的客户端可以访问服务的所有页。对象名、nonce 和 token 用于关联与重放隔离，不是密码学认证或每个应用的访问授权。信任边界仍为受控 LAN，继承现有集群协议，不适合作为敌对租户之间的内存隔离机制。

## 6. 运行检查

```sh
make check-cluster-memory
make check-cluster-memory-network

# 复用明确指定的既有 BOTH ABI 内核/磁盘，只重新构建本阶段的用户程序。
python3 tools/check-cluster-memory-network.py \
  --kernel /absolute/path/kernel.elf --image /absolute/path/fat32.img
```

第一项运行生产 Rust 状态机（1/8/64 页容量）、并发写者竞争、事务期限和重放、 10 万畸形消息变异，以及 C 配置解析/FFI/SDK 的 ASan/UBSan 检查。Rust 本身由普通 rustc 编译，并未声称整份 Rust 静态库都受 C 编译器的 sanitizer 插桩。

第二项目前是 x86_64 双独立 QEMU；默认 KVM，可用 `--accel tcg`。分别检查 off/read-only/read-write，运行真实服务进程；客户端通过 UDP 访问，服务节点自身再用本地客户端提交反向修改，并覆盖暂存不可见、提交重放、过期、取消和实际服务重启。两机 RAM 完全分离，宿主只转发 Ethernet 帧，不生成业务回复。磁盘使用每次运行的副本和独立 snapshot 写层，不修改输入镜像。结果、启动参数和输入哈希保存在 `.kernel-build/cluster/memory/network-*/report.json`。

## 7. 与透明线程执行的关系

新增的 [透明线程执行片](64-transparent-thread-slices.md) 接入普通 pthread、真实缺页、用户上下文与唯一执行权、home 系统调用代理。它使用独立的`CLUSTER_SSI` 开关和进程配置，当前为 x86_64/UP 串行交接，不提供同进程多机并行加速，也不调用本服务的显式页 SDK。

完整并行 SSI 仍需要页所有权/撤销/TLB 协议、分布式原子和内存序、多个 worker 的数据位置调度、完整信号/异常与资源视图、分区 fencing 和恢复。不能从本服务的快照与事务推断已经具备这些语义。

两个模块的开关独立：本服务 `mode off` 不会关闭已启用的 SSI 线程策略， SSI 的 `shared-memory 0` 则只禁止该进程的透明跨机线程执行，普通线程留在同机；显式远程页和既有远程应用/作业仍按各自配置工作。
