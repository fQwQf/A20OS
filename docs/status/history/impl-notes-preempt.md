# impl-notes-preempt：内核抢占（CONFIG_KERNEL_PREEMPT）落地与实测

实施：主会话规格定稿 + space-bunny 子代理分阶段实施（核心层 / 逐架构钩子 / 集成），
主会话复核并修正三处（见 §5）。机制设计论证：`docs/process-scheduler.md` §4.1；
锁语义叠加：`docs/roadmap/lock-serialization-split.md` §9。
落地提交：`0d05c0726 feat: kernel preemption`、`b2d2dc8f3 fix: guard the JVM
SIGSEGV register/object dumps behind __x86_64__`（合并进 main 于 `d1aa68c3c`）。

## 0. 落点

- 核心层：`kernel/include/core/preempt.h`、`kernel/core/preempt.c`（per-CPU
  抢占计数 + hardirq 标记）、`kernel/include/core/lock.h`（三处锁挂钩）、
  `kernel/core/trap.c`（IRQ 返回判定点 + 缺钩子 `#error`）、
  `kernel/proc/sched.c`（`kernel_preempt_at_irq_return()`、sched() 切换段
  关中断、`context_switch_locked()` 计数断言）、`Makefile`（开关 + `-preempt`
  变体标记）。
- 架构钩子（每架构一处宏，读保存帧里的"被中断上下文中断使能位"）：
  x86_64 `rflags.IF`、riscv64/32 `sstatus.SPIE`、aarch64 `SPSR.I`、
  loongarch64/32 `PRMD.PIE`、arm32 `CPSR.I`、ppc64le `SRR1.MSR.EE`。
  逐架构的栈属性证据（内核态 IRQ 落在任务自身内核栈）与位号依据在各
  `kernel/arch/<arch>/include/` 头文件注释中。

## 1. 验证矩阵（合并前）

| 架构 | check-*-bringup | smoke-* | 备注 |
|---|---|---|---|
| x86_64 | PASS | PASS | 行为测试见 §2 |
| riscv64 | PASS | PASS | 行为测试见 §2 |
| aarch64 | PASS | PASS | |
| riscv32 | PASS | PASS | |
| loongarch64 | PASS | PASS | |
| ppc64le | PASS | PASS | 含 trap 分流修复后重跑 |
| loongarch32 | 未跑 | 无目标 | 本机无 la32 工具链（文档注明 local-only）；钩子经宿主 gcc 预处理验证展开为 `prmd & (1UL<<2)` |
| arm32 | 未跑 | 无 | 钩子经预处理验证展开为 `((ctx)->cpsr & (1U<<7)) == 0`；构建阻塞见 §5.1 |
| armv7m | 设计排除 | — | MCU PendSV 协作模型，不编入 |

另：`smoke-smp-bringup`（NR_CPUS=2）PASS、`smoke-abi-linux` PASS。

## 2. 行为测试（RT 唤醒延迟 A/B 探针）

`user/cmds/core/preempt_lat.c`：hog 子进程循环 `read()` 256MB page-cache 热文件
（TCG 下单次 446-608ms，页间无重调度点），RT 子进程 `SCHED_FIFO(10)` 睡眠 20ms
× 20 次报告迟到量。判定线：任一架构 max < 100ms 即判生效。x86_64 经
`tools/run_instance_tcg.py` 强制 TCG（KVM 下长 syscall 前提不成立），riscv64 走
`tools/a20 test`。

| 配置 | hog 单次 read | RT max | RT 均值 | 说明 |
|---|---|---|---|---|
| x86_64 smp1 抢占开 | 577-595ms | **10 / 11 / 21 ms**（3 次干净跑） | — | 1-2 tick |
| x86_64 smp1 抢占关（对照） | 577ms | **557ms** | — | 前 10-13 样本稳定在 539-557ms，等于 hog 当次 read 剩余时长——"不抢占"指纹 |
| x86_64 smp2 钉 CPU0 | 608ms | **1ms** | 0ms | 20 样本全部 ≤1ms |
| x86_64 smp2 自由 | 591ms | **0ms** | 0ms | |
| riscv64 smp1 抢占开 | 446-492ms | **1 / 11 / 31 / 59 ms**（4 次） | — | 59ms 仅冷启动首样本 |
| riscv64 smp1 抢占关（对照） | 447ms | **427ms** | — | 前 13 样本 411-427ms |
| riscv64 smp2 钉 CPU0 | 521ms | **10ms** | 1ms | |
| riscv64 smp2 自由 | 462ms | **10ms** | 2ms | |

判据核查：`sched_setscheduler_rc=0 policy=1`（FIFO 生效）、SMP 轮 `pin_rc=0`
（`sched_setaffinity` 生效）。

smp=2 优于 smp=1 的原因（读码结论，非独立测量）：等待定时器堆被两个 CPU 的
tick 交错扫描（`proc_sched_tick` → `proc_sched_expire_wait_timers`），等效唤醒
粒度减半；跨 CPU 唤醒经 resched IPI 在目标 CPU 的 IRQ 返回点抢占。

## 3. 复核中修正的三处

1. **cow.c 守卫**：`mm_fork_page_class()` / 两处 `mm_pt_sync_status()` 调用
   未受 `ARCH_HAS_PGTABLE_OPS` 守卫（基线 `9b7944da6` 即编译失败，非本特性引
   入）。按 fault.c 既有惯例守卫，arm32 实测编译通过该文件。
2. **ppc64le trap 分流**：`ppc64_trap_dispatch()` 原先把所有 trap 交给
   `trap_handler()`（用户路径），内核态 IRQ 到不了抢占判定点，且内核 tick 被
   记为用户态时间、内核上下文上可能构造用户信号帧。改为按 `SRR1[PR]` 分流到
   `kernel_trap_handler()`，`smoke-ppc64le` 重跑 PASS。
3. **`#error` 加固**：原 `#warning` 是死代码（`core/arch.h` 弱默认无条件定义
   钩子宏），架构漏写钩子会静默永不抢占。移除弱默认、升级为硬错误；六架构
   构建通过即证明钩子全部就位。

## 4. 待验证 / 未覆盖（诚实清单）

- **arm32 构建仍红**：`kernel/mm/{fault,mmap,mm,mprotect,munmap,elf}.c` 约 30 处
  per-PTE status sidecar 调用未守卫（"one mapping record" 重构遗留，基线即红，
  与抢占无关）。修法同 §5.1 的 cow.c 模板；修复后 arm32 的抢占验证（构建+冒烟）
  才能补上。
- **loongarch32**：待 cloudspurs la32 工具链（`docs/platforms/loongarch32.md`）。
- 行为测试只覆盖"单个长 syscall 被抢占"场景；"两 CPU 各忙时 RT 的放置策略"
  未单独测量（钉扎轮已证明竞争路径正确）。全部对照只跑 1 次，无置信区间。
- 一次 x86_64 smp1 抢占开跑出 max=183ms 离群值，当时宿主机在跑 16 路 `make -j`；
  空载重跑 3 次均 ≤21ms。根因未定位，标记为宿主机负载相关疑点。
- 锁热路径新增两次原子加减未做性能基准对比。
- KVM 下未测（前提不成立）；SMP 行为数字见 §2，SMP 稳定性由 `smoke-smp-bringup`
  覆盖。
