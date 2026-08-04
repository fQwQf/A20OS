# 混合内核改造实施路线

设计总纲见 [00-design.md](00-design.md)。每个阶段有独立验收标准，
达到后才进入下一阶段；每阶段完成后更新本文档状态列。

## 状态总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1 | `channel_call` 融合 RPC 快路径（内核 + SDK + 基准） | 进行中 |
| 2 | svcman 服务监管者 + 崩溃自愈演示 | 未开始 |
| 3 | 共享 VMO 环形队列 uapi 协议 + 数据面基准 | 未开始 |
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
