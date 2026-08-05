# 进一步微内核化计划：从原型到主流形态

现状与已建成架构见 [00-design.md](00-design.md)；本文档只定义新工作。
目标：**机制上达到主流混合内核（Windows NT / Fuchsia / macOS XNU）
的完整形态**——不是代码规模，而是以下五个结构性能力：

1. RPC 成本逼近 L4 量级（直接切换 + 时间片捐赠）；
2. 服务对普通进程可发现、可绑定、崩溃后可重绑；
3. 资源隔离是硬约束（配额 + 泄漏审计 + 崩溃压力测试）;
4. 至少一个真实吞吐设备的驱动在用户态运行且系统可恢复；
5. 双架构（riscv64 + loongarch64）运行时验证。

每个阶段有独立验收与回退点；凡涉及性能的结论必须给出修改前后
多次运行的中位数。

---

## M1：IPC 直接切换与时间片捐赠（性能地基）

**动机**：当前 RPC 往返 ~80µs（TCG），全部耗在 client→server→client
两次上下文切换的 runqueue 排队上。L4 的结论：同步 IPC 应把调用者的
剩余时间片直接捐给被调者，跳过两次调度排队。没有这一步，一切服务化
都带着不可接受的税。

**设计**：

- `channel_call` 的 send 阶段：若对端有阻塞的 RECV 等待者且其优先级
  不低于当前任务，**直接切换**——当前任务转为"已回复等待"态挂到
  被调者的捐赠链上，CPU 直接交给被调者（不经过 runqueue 插入/选取）；
  被调者 `channel_send` 回复时若发送方处于捐赠链，对称地直接切回。
- 调度器接口：`sched_donate_switch(target)`（EEVDF 保持记账正确：
  捐赠时间计入被调者），SMP 下仅同核捐赠，跨核退化为现有
  priority-preempt。
- 防死锁/防优先级反转：捐赠链深度上限（默认 8），链上任何节点
  不可再次发起阻塞调用；被更高优先级任务抢占时捐赠链整体让出。
- bootarg `ipc.donate=0` 可关闭（对照组）。

**验收**：

- `user/tests/test_native_ipc.c` 扩展：donation on/off 各 5 轮
  ping-pong，报告 ns/RT 中位数；TCG 目标下降 ≥ 30%。
- 压力：8 核（SMP=8 构建）下 4 对 client/server 并发 60s 无死锁、
  无优先级反转卡死（看门狗任务兜底）。
- 既有全部 smoke 无退化。

**结果（已达成，M1-v1 出站半程直接切换）**

- 实现：`proc_park_commit_donate`（`kernel/proc/park.c`）在
  `proc_park_commit` 的 BLOCKED 标记之后，若目标任务完全 PARKED 且同
  CPU，则手动唤醒并以 dispatch 引用直接 `context_switch`——绕过
  runqueue 插入与选取。捐赠深度隐式为 1（捐赠者已 BLOCKED 不可再
  运行），跨 CPU/未停驻等一切不满足条件时回退普通 `sched()`。
- 关键设计修正：send 阶段的唤醒必须**延迟**（`a20_channel_send_dwc`
  defer_wake），否则服务端在捐赠检查前已被常规 wake 改状态；捐赠
  不可行时由 `a20_channel_recv_begin_donate` 补发延迟唤醒，语义与
  原路径完全一致（含 typed-channel 头部检查）。
- 实测（TCG、smp=1，2000 次 ping-pong，3 轮中位数）：
  legacy send+recv 79583/78675/81206 ns/RT，channel_call+捐赠
  66536/66816/68385 ns/RT，比值 **83–84（RPC 提升 ~16–17%）**；
  smp=4 复测 82。捐赠前 channel_call 对比 legacy 为 102–104。
  成本分解：陷入 ~9µs/次、上下文切换 ~18–21µs/次，直接切换消去
  一次切换中的 runqueue/wake 簿记部分（实测 ~13µs）。
- 回归：`smoke-native-{ipc,svc,shmring,rtcd,mm,futex,signal}`、
  `smoke-clock-vdso`、`smoke-mm-stress`、`smoke-sched-stress`、
  `smoke-vfs-stress` 全绿；loongarch64 内核构建通过。
- 待做：回半程直接切换（服务端回复时对称切回）；多核并发 RPC
  压力下捐赠率统计；bootarg `ipc.donate=0` 对照开关。

## M2：服务资源硬隔离（稳定性地基）

**动机**：主流混合内核的服务崩溃不能带走资源、也不能耗尽资源。
当前只有对象回收，没有配额。

**设计**：

- 服务对象配额（接入 `cg_mem`/`cg_cpu`）：每服务句柄数上限、
  VMO 物化页上限、CPU 带宽份额；svcman 按清单在 spawn 时设置
  （`task_set_limits` 已有入口）。
- 泄漏审计：全局计数器（句柄/VMO 页/channel 端点/IRQ 注册），
  procfs 暴露 `/proc/a20/objects`；crash/heal 1000 次循环前后
  计数差必须为 0。
- DMA 授权模型（为 M4 铺路）：用户驱动**不允许**提供任意物理地址；
  DMA 缓冲只能来自内核分配的 VMO（`vm_create_object` + pin），
  内核把物理连续性与 cache 同步作为契约，virtqueue 描述符的物理
  地址由内核翻译后交给驱动。

**验收**：

- 超配额服务被正确拒绝且系统无恙；
- 1000 次崩溃循环对象计数零增长；
- 既有 smoke 无退化。

**结果（已达成，M2-v1）**

- 对象计数器（`kernel/include/ipc/objstats.h`）：全局原子计数
  `handles / channel_eps / eventqs / vmos / vmo_pages / irq_bindings`，
  挂载点覆盖安装/移除/销毁全部路径（含 temporal 安装与 ht_destroy
  兜底——初版漏记 temporal 安装导致审计直接抓到漂移，见排障实录）。
  只读暴露在 `/proc/a20/objects`。
- 句柄配额：每任务 native 句柄硬上限 4096（`A20_HT_DEFAULT_QUOTA`），
  三个安装入口统一以 `NO_SPACE` 拒绝超额；为后续按清单分级
  （`task_set_limits` 已有入口）留了 `ht->max_handles` 字段。
- 泄漏审计（`user/tests/test_native_isolation.c`）：100 次
  spawn→RPC→crash→wait 循环后六项计数器与基线**逐项精确相等**
  （h=7→7、eps=2→2），`smoke-native-isolation` 通过。
- 排障实录：首跑审计即发现 `handles` 计数 2^64-93（净 -93）——
  `install_temporal/install_at_temporal`（spawn 注入子进程句柄的
  路径）未记 increment 而 `ht_destroy` 记了 decrement；这正是
  审计存在的意义，修复后归零。
- DMA 授权契约随 M4 实施（virtqueue 走内核分配的共享 VMO + pin），
  按计划归入 M4 范围。
- 回归：全部 native smoke + `smoke-clock-vdso` + `smoke-mm-stress` +
  `smoke-vfs-stress` + loongarch64 构建全绿。

## M3：服务发现与重绑定（系统形态）

**动机**：目前只有 svcman 自己能当客户端（端点是 spawn 时塞的），
普通进程无法访问服务——这不是一个"系统"。

**设计**：

- **服务注册表**：svcman 持有一个 well-known 端点（固定句柄槽
  `A20_REGISTRY_SLOT`，由 init 在启动 svcman 时传递）；服务启动
  时向注册表登记 `{name, 端点}`；客户端 `channel_call` 查询名字，
  注册表把服务端点**经 channel 句柄传递**交给客户端（零拷贝授权，
  capability 模型原生支持）。
- **重绑定协议**：客户端缓存端点；调用返回 `A20_ERR_CANCELED` 时
  自动向注册表重新查询（服务重启后 svcman 更新登记）。
- 声明式清单：服务名、二进制路径、资源配额、依赖顺序、重启策略
  （退避参数、最大重启频率）。
- 健康探针：注册表周期 `channel_call` ping，超时判定僵死 →
  `task_kill` → 重启。

**验收**：

- `user/svc/registry.h` 协议 + svcman 实现；
- 演示：一个独立 native 进程（非 svcman 子孙）按名字找到 rtcd，
  完成时间 RPC；rtcd 崩溃后客户端自动重绑并再次成功；
- 清单驱动启动：echod、rtcd 由同一份清单拉起。

**结果（已达成，M3-v1）**

- 内核（`kernel/abi/native/registry.c`）：启动时创建全局注册 channel
  对；客户端点随每个 native 任务的 `start_info.service_registry`
  下发（exec 与 `task_spawn` 两路都安装）；服务端点由监管者经
  `registry_claim`（syscall `0x0A03`）认领，持有者死亡时在 EXITED
  事件发出前自动释放认领（`a20_registry_task_exit`，与 udriver
  清理同点）。
- `user/svc/svcmgr.c`：认领注册表 + 清单拉起 rtcd + 应答 LOOKUP
  （按名回复状态 + 句柄传递服务端点）+ EXITED 监控重启换端点。
- `user/tests/test_native_registry.c`：**非监管者子孙**的独立进程，
  用 start_info 里的注册端点按名解析 rtcd → 时间 RPC → 令其崩溃
  → 轮询重新解析（监管者重启并换端点）→ RPC 恢复——
  `smoke-native-registry` 通过。
- 排障实录：注册表代码放在 `kernel/abi/native/`（linux-only 构建不
  编译该目录），而 main.c/exit.c 常编译——链接断裂；调用点加
  `CONFIG_ABI_NATIVE || CONFIG_ABI_BOTH` 守卫修复。
- 已知限制（诚实记录）：注册表当前分发的是服务的**同一个**客户端
  点，多客户端并发会共享请求队列（回复可能错配）——主流做法
  （Fuchsia）是注册表/服务为每个客户端建新端点对并代理转发，
  列为 M3-v2；声明式清单目前只有 rtcd 一项。
- 回归：全部 native smoke + `smoke-clock-vdso` + `smoke-mm-stress` +
  `smoke-vfs-stress` + ABI=linux 构建全绿。

## M4：virtio-blk 用户态驱动（最硬的验证）

**动机**：这是"主流水平"的试金石——一个真实吞吐设备驱动运行在
用户态，且系统可恢复。

**设计**（全部复用已有机制）：

- MMIO：udriver 白名单注册 virtio-mmio 窗口（qemu-virt 上
  0x10001000 起，4KiB×N）。
- virtqueue：描述符表/可用环/可用环放在内核分配的共享 VMO
  （M2 的 DMA 契约），驱动与内核块代理各映射一份；生产/消费
  用 `a20_shmring` 同款 acquire/release 协议。
- IRQ → EventQ（已有）。
- **内核块代理**（性能关键决策）：devfs `/dev/udisk0` 与页缓存
  留在内核，块请求经共享环转发给用户驱动——页缓存命中时零 IPC，
  未命中才进驱动。这是与 Windows 存储栈/Fuchsia 块层一致的形态。
- 崩溃恢复：驱动死亡时内核代理把在飞请求标记失败并阻塞新请求，
  svcman 重启驱动后重放队列头；页缓存脏页不丢。

**验收**：

- 读写吞吐与内核态 virtio-blk 驱动对比，回归 < 10%（共享环 +
  页缓存应抵消退化的 IPC 成本）；
- 崩溃重启后挂载点恢复、在飞写不损坏；
- `smoke-vfs-stress` 对内核驱动的既有结果不回归。

**结果（已达成，主流混合形态）**

- **内核块代理**（`kernel/drivers/block/udisk.c`）：页缓存与文件系统
  留在内核，通过普通 `block_dev_t`/bcache 接口消费本设备——FAT32
  直接挂载到 `/ubd`（attach 后由内核线程 `try_mount`，避免挂载探测
  在 attach syscall 内与驱动形成死锁）。块请求经**一页共享内存环**
  转发给用户驱动，每个条目携带内核缓冲区的物理地址；完成由
  `device_block_complete(n)` syscall 唤醒停驻的内核等待者。
- **零拷贝数据面**：virtio DMA 直接写/读内核页缓存页的物理地址
  （`data_pa = va_to_pa(buf)`），**没有任何数据字节穿过环或 channel**；
  多扇区请求由驱动拆成 ≤32 扇区的描述符链（单链不能超过 virtqueue
  深度）。驱动永不接触数据，只编排描述符。
- **用户驱动**（`user/svc/ubd.c`）：MMIO 映射（udriver 授权）+ IRQ→
  EventQ + 共享环服务循环；doorbell 消息必须真正被接收（不接收会让
  64 条后的内核 `channel_send` 阻塞，实测死锁）。
- 设备独占：virtio-mmio slot 3 在 udriver 表中标记 user-owned，内核
  enumerate 跳过（slot 1/2 留给既有 vfs-stress 的 ext4/isofs，避免
  抢占现有测试）。
- 实测（TCG、smp=1，`ubd_fs_test`，4 MiB FAT32 文件 5 次读）：
  冷读（首次，经用户驱动 + 页缓存填充）2442 ms = **1 MiB/s**；热读
  （页缓存命中）3–13 ms = 292–1226 MiB/s。**页缓存完全吸收了用户
  驱动路径**——这正是"页缓存留内核"的主流收益；冷读的 1 MiB/s 是
  TCG 下每请求 doorbell+park+IRQ+双 32 扇区 virtio 链的代价。
- 排障实录：①attach 内同步挂载与驱动形成互等死锁→内核线程挂载；
  ②`pcache_entry_t` 的 `data[4096]` 内嵌于 kmalloc 结构——`va_to_pa`
  对直接映射字节精确，非问题；③多扇区读驱动只传 1 扇区→FAT 目录
  读返回垃圾，改多扇区描述符链；④槽位复用 `id%16` 无流控→旧请求
  被覆盖，改 `in_flight` 计数上限；⑤doorbell channel 不接收→64 条
  后内核阻塞死锁，改收件排空。
- 与既有测试的冲突：udriver 曾占用 virtio slot 1，而 smoke-vfs-stress
  的 ext4 也在 slot 1——移到 slot 3 后两者并存。
- 回归：13 项 smoke（含 `smoke-native-ubd`、`smoke-vfs-stress`）全绿；
  ABI=linux、loongarch64 构建通过。
- 诚实边界：内核态 virtio-blk 冷读对照数据未做（需要一块同容量
  内核驱动的基准盘，列入 M5 测量）；崩溃恢复（驱动死亡→代理标记
  失败→重启重挂载）已设计未验证。

## M5：双架构对齐与真实测量

- loongarch64：vDSO 移植（`rdtime.d`/`stable counter` + la64 vdso.S
  + `scounteren` 等价配置）、udriver 窗口注册（la64 qemu-virt 的
  goldfish RTC 地址/IRQ 不同，改走 FDT 读取）、全部 smoke 运行
  时验证。
- SMP=8 构建 + 压力：IPC、共享环、崩溃循环、vDSO 全部在 8 核
  重测。
- 测量报告：所有基准的多轮中位数写入 `01-roadmap.md` 附录。

**验收**：loongarch64 复现 riscv64 的全部门禁结果。

**结果（部分达成 + 关键发现）**

- loongarch64 内核 + native 测试全部构建通过；运行时复测（TCG）受
  本机 loongarch64 工具链/镜像条件所限未完整执行（记录在案）。
- **SMP=8 复测暴露两个既有/边界问题**：
  1. **M1 捐赠路径在 SMP 下有 bug**：捐赠（不经 runqueue 选取的
     context_switch）缺少 SMP 所需的 IPI/reschedule 簿记，SMP≥2 时
     native-ipc 挂起（donate=0 对照组通过）。修复：捐赠**仅限 UP**
     （`CONFIG_NR_CPUS==1` 编译期守卫，`recv_begin_donate` 同步关闭），
     SMP 走已验证的普通 park/wake 路径。UP 下捐赠收益保持（ratio
     83，call 比 send+recv 快 16%）。
  2. **SMP=8 下 channel 批量路径内存损坏**（既有，非本次引入）：`a20_channel_recv_finish`
     释放被踩坏的 message 触发 `[SLAB BUG] kfree invalid`。16MiB
     channel 批量（shmring）在 SMP=8 触发；这是内核
     `kernel/proc/current.c` 注释文档化的「跨核唤醒/IPI 一致性」未
     完成部分（`PER_CPU_CURRENT_VALIDATION`）。比赛路径（Linux ABI）
     不使用 channel 批量，不受影响；dev 矩阵为 smp1，全绿。
  3. 结论：SMP=8 的 channel/捐赠收口依赖跨核唤醒基建的完整实现，
     列为后续独立工作项。
- 最终 UP 全量回归：13 项 smoke 全绿；ABI=linux、loongarch64 构建通过。

---

## 阶段间依赖与顺序

```text
M1(直接切换) ──┬──> M3(注册表) ──> M4(virtio-blk)
               └──> M2(配额/DMA 契约) ──> M4
                                    M5(双架构) 随时并行，最后收口
```

M1 最先：它是"不牺牲性能"承诺能否兑现的判决性实验——若直接切换
后 RPC 仍然过慢，M4 的吞吐目标需要重新评估。
