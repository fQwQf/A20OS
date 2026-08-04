# 混合内核改造实施路线

设计总纲见 [00-design.md](00-design.md)。每个阶段有独立验收标准，
达到后才进入下一阶段；每阶段完成后更新本文档状态列。

## 状态总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1 | `channel_call` 融合 RPC 快路径（内核 + SDK + 基准） | 完成（riscv64） |
| 2 | svcman 服务监管者 + 崩溃自愈演示 | 未开始 |
| 3 | 共享 VMO 环形队列 uapi 协议 + 数据面基准 | 未开始 |
| 3B | Linux ABI 透明桥接：pipe over VMO 环 + vDSO | 未开始 |
| 4 | 低速驱动外迁试点（用户态驱动框架） | 未开始 |
| 5 | 块/网驱动外迁评估（按基准数据决策） | 未开始 |

## 阶段 1：`channel_call` 融合 RPC 快路径

**改动面**

- `kernel/include/abi/native/types.h`：`a20_channel_call_args_t`（版本化头）。
- `kernel/include/abi/native/syscall_nr.h`：IPC 号段新增 `channel_call`。
- `kernel/ipc/a20_channel.c`：`a20_channel_call()`——一次陷入完成
  入队 + 等待回复；入队时检测对端 RECV 等待者并走 priority-preempt 唤醒。
- `kernel/abi/native/syscall_table.def`、`sys_native_ipc.c`：syscall 入口。
- `user/liba20rt/a20_channel.h`：用户态内联封装。
- `user/tests/test_native_ipc.c`：ping-pong 往返延迟基准
  （channel_call vs send+recv 对比）+ 功能正确性（句柄随 call 传递、
  NONBLOCK、对端关闭）。

**验收**

- `make native-ipc-test-rv` 构建通过；`make smoke-native-ipc` 在 QEMU
  riscv64 输出 `NATIVE_IPC: PASS` 并打印往返延迟对比。
- 既有 `smoke-native-*` 全部无退化。

**结果（已达成）**

- 实现：`sys_a20_channel_call`（`kernel/abi/native/sys_native_ipc.c`），
  syscall 号 `0x0508`；SDK 封装 `a20_channel_call[_flags]`
  （`user/liba20rt/a20_channel.h`）。单次陷入完成 request+reply 等待，
  单次句柄查找（READ|WRITE）+ 单次参数校验。
- 功能测试：RPC 回显、双向句柄传递、NONBLOCK 语义、对端关闭
  CANCELED，全部通过。
- 基准（QEMU TCG、smp=1、2000 次往返）：send+recv 80818 ns/RT，
  channel_call 83130 ns/RT，比值 102%。TCG 下每次陷入/上下文切换
  开销被模拟器放大，融合省去的 1 次陷入 + 1 次句柄查找淹没在
  两次上下文切换的成本中；真实硬件上陷入占比更高，融合才有可测
  收益。结论：机制正确、性能中性，符合"不牺牲性能"底线。
- 排障记录：内核严格校验「handle_count=0 且 handles 指针非空」
  返回 `-EINVAL`（`sys_native_ipc.c` send 路径），RPC 服务端回复
  时必须传 NULL 指针而非空数组——已固化进测试注释。
- 回归：`smoke-native-futex/mm/signal` 全绿。
- 待做：真实捐赠式直接切换（L4 direct switch）留作后续优化；
  loongarch64 构建验证（`native-ipc-la` 目标已提供）。

## 阶段 2：svcman 服务监管者

**改动面**

- `user/liba20rt/a20_service.h`：服务端骨架（channel 服务循环 + 心跳应答）。
- `user/svc/svcman.c`：监管者。静态清单拉起服务（`task_spawn` v2，
  stdio 继承 console）；EventQ watch 每个服务的 TASK handle
  （`A20_EVENT_EXITED` 已存在，`kernel/proc/exit.c:363`）；指数退避重启；
  重启次数与最后退出码通过 stdout 汇报。
- `user/svc/echod.c`：演示服务（channel_call 回显 + 收到 "crash" 消息
  时故意退出，用于演示自愈）。
- `user/tests/test_native_svc.c`：端到端验证——查询 echo 服务、令其崩溃、
  断言重启后服务恢复。

**验收**

- `make smoke-native-svc`：日志依次出现服务就绪、崩溃检测、重启、
  二次请求成功，最终 `NATIVE_SVC: PASS`。

## 阶段 3：共享 VMO 环形队列

**改动面**

- `user/liba20rt/a20_shmring.h`：user/user（后续 user/kernel）共享的
  SPSC 环协议头——相对偏移、acquire/release、doorbell 标志。
- `user/tests/test_native_shmring.c`：两个 native 任务经 channel 交换
  VMO 句柄后做批量数据传输吞吐基准，与 channel 大数据消息对比。

**验收**

- `make smoke-native-shmring` 输出 `NATIVE_SHMRING: PASS` 与吞吐对比。

## 阶段 3B：Linux ABI 透明桥接

让未修改的原生 Linux 程序共享混合内核收益（设计见 00-design.md §7）。
顺序按收益/风险比排列，每步独立验收：

1. **唤醒捐赠核对**：确认 pipe/AF_UNIX/futex 的 wake 全部走
   `proc_try_wake` priority-preempt；补齐遗漏点。零风险，两个 ABI
   自动共享。
2. **vDSO**：exec 装载只读 vDSO 页 + auxv `AT_SYSINFO_EHDR`；
   先实现 `clock_gettime(CLOCK_MONOTONIC)` 与 `gettimeofday`。
   验收：Linux 用户态基准（musl `clock_gettime` 循环）陷入次数降为零；
   与内核 syscall 结果交叉校验单调性与精度。
3. **pipe over VMO 环**：`kernel/fs/pipe.c` 数据面替换为 VMO-backed
   SPSC 环。验收：`pipe_stress`/`vfs_stress` 全过；大块管线吞吐
   （如 `cat bigfile | wc -c`）不劣于旧实现。
4. **AF_UNIX over Channel（评估后决策）**：SOCK_STREAM 数据面重建于
   Channel；SCM_RIGHTS 映射到句柄传递。仅在 1–3 的基准证明底层
   快路径成熟后进行。

**统一验收**：每条改动前后跑同一组 Linux ABI 基准（`socket_stress`、
pipe 吞吐、`clock_gettime` 延迟），中位数不允许回归；既有
`smoke-native-*` 与 Linux ABI smoke 全绿。

## 阶段 4：低速驱动外迁试点（规划要点）

- 内核侧新增"驱动授权"：把指定 MMIO 物理区间包装为 DEVICE handle
  （带 MAP right）+ 把 IRQ 等待暴露为 EventQ 事件源。
- 选择一个低速设备（候选：RTC 或 virtio-input）整体迁入用户态服务，
  devfs 节点由内核代理转发到服务 channel。
- 验收：功能等价（现有读 RTC/输入测例全过）+ 杀死驱动服务后系统存活、
  服务重启后设备恢复 + I/O 延迟回归 < 5%。

## 阶段 5：按数据决策

仅在阶段 1–4 的基准证明 IPC/环开销可接受后，评估块/网驱动外迁；
不达标则保持内核态驱动，混合内核形态停留在"服务化 + 低速驱动外迁"。

## 通用纪律

- 每个关键改进前先 commit；commit 信息格式 `<scope>: <summary>`（英文）。
- 新增 native 测试一律纳入 `NATIVE_OUTPUTS`，使其进入 fat32 镜像 /bin。
- 正式性能结论只来自 QEMU smoke 日志的多次运行中位数。
