# 1.0 交付基线

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

本页记录 1.0 交付工作包（V1-DELIVERY）确定的**基线**、干净候选的启动验收、保留平台
构建矩阵、SMP 门禁与测试基线的分类，以及配置好的 CI 工作流逐 job 的当前状态。
结论都来自当前提交上的实际重跑；运行产物不入库。

## 1. 基线取在哪

`main` 工作树在本轮开始时带有未提交改动。这些改动跨多个领域、在本次会话里没有归属者，
也没有经过 review，因此**不进入基线**，原样保留在 `main` 工作树（不提交、不丢弃、不 stash）。
基线取当前提交的干净树；本包在独立 worktree/分支 `codex/v1-delivery` 工作，没有在 `main`
上改动任何文件。

判定依据不是"改动看起来像谁的"，而是两个可核对的事实：干净树在没有这些改动时**能重建内核
与用户态镜像并通过 shell 门禁**（§2），而它们既不关闭本台账任何条目，也不被本台账引用。
逐项归属如下（按领域合并）：

| 未提交改动 | 领域 | 判定 |
| --- | --- | --- |
| `kernel/arch/aarch64/boot/entry.S`、`kernel/arch/aarch64/trap/trap.S`、`instances/xfce-aarch64.toml`（`smp = 4` → `1`） | DESKTOP | 保留原状：AArch64 桌面/JVM 的 EL0 访问控制与 SMP4 规避，属于 DESKTOP 台账 |
| `packages/overlay/xfce/**`、`tools/fetch-minecraft.sh`、`tools/pkg.py`、`tools/a20-fortify.c`、`tools/a20-fortify.map`、`instances/minecraft-*.toml`、`instances/xfce-loongarch64.toml` | DESKTOP | 保留原状：Minecraft 多架构 natives 与 glibc-fortify 兼容层（其中新增文件不存在于本轮基线树里） |
| `kernel/arch/loongarch64/include/{cpu.h,trap_frame.h}`、`kernel/arch/loongarch64/trap/trap_bridge.c`、`kernel/core/trap.c`、`kernel/include/core/arch.h` | CORE / HYP 边界 | 保留原状：LoongArch 假中断的丢弃路径 |
| `kernel/core/smp.c`、`kernel/include/mm/frame.h` | CORE / MM | 保留原状：等待窗口注释与 IRQ 状态归还 |
| `user/cmds/core/{fsync_durability_test.c,jbd2_retry_test.c,ufs_mount_state_test.c}`、`user/cmds/stress/mm_stress_phase_diag.c` | FS / MM | 保留原状：新增/改写的持久化与 MM 诊断用例（其中新增文件不存在于本轮基线树里） |
| `tools/rust-user.mk`、`tools/tests/test_stamps_clean.py` | 构建 / 测试 | 保留原状：RV32/x86_64 Rust peer 的在途工作（本轮另行独立修复了同一构建里的目录与锁定位缺陷，见 §5） |
| `tools/a20` | 交付工具 | 保留原状：CLI 参数解析与 QEMU 前置门禁的在途工作；本包未改 `tools/a20` 的行为 |
| `tools/rust_idl_peer_wire_check.c` | ABI | 保留原状（该文件不存在于本轮基线树里） |
| `tools/snapshot_build_inputs.py` | 交付工具 | 保留原状，且**不纳入 1.0**：它按定义生成带哈希的构建输入清单，与"运行产物、哈希清单不入库"的当前约定冲突（该文件不存在于本轮基线树里） |

该基线**不冻结**任何外部 ABI 或持久化格式。

## 2. 干净候选与启动验收

重建输入（本机实测版本，全部来自宿主发行版包）：

| 组件 | 版本 |
| --- | --- |
| `riscv64-unknown-elf-gcc`（裸机内核） | 14.2.0 |
| `riscv64-linux-gnu-gcc` / `aarch64-linux-gnu-gcc` / `x86_64-linux-gnu-gcc` / `loongarch64-linux-gnu-gcc` | 14.2.0 |
| `arm-linux-gnueabihf-gcc` / `arm-none-eabi-gcc`（仅固定 CI 容器） | 14.2.0 / 14.2.1 |
| `qemu-system-riscv64` | 10.0.13 |
| `rustc` | 1.96.0 |
| `python3` / `make` | 3.13.5 / GNU Make 4.4.1 |

构建与门禁命令：`tools/a20 build qemu-riscv64`（展开为
`make ARCH=riscv64 BOARD=qemu-virt-riscv64 dev-build`），随后按实例配置启动 QEMU 并驱动
串口。

结果：内核、FAT32/ext4/ISOFS 镜像与用户态在干净树上重建成功；普通 shell 门禁通过——
进入 shell、执行内置命令与外部程序（`/bin/echo`）并看到输出、`poweroff` 后内核打印
关机序列且 QEMU 以 0 退出。这是启动验收，不是模块功能证明。

## 3. 保留平台构建矩阵

工具链档位与运行证据分开记录。`build` 指该档位的内核 bring-up 与用户态构建通过；
`runtime` 指在 QEMU 里真实启动并自然退出；`N/A` 指该档位不适用；**缺工具链或无板一律记为
待验证/失败，不记为 PASS**。宿主没有的 `arm-linux-gnueabihf-gcc` 与 `arm-none-eabi-gcc`
在固定 CI 容器（`ghcr.io/fqwqf/a20os-buildenv`）里补齐后复跑。

| 架构 | build | runtime | 备注 |
| --- | --- | --- | --- |
| riscv64 | 通过 | 通过 | §2 的干净候选；`smoke-arch-mmu-matrix` 的 riscv64 变体也覆盖 MMU 启动 |
| aarch64 | 通过 | 未验证 | 构建通过；桌面实例的运行证据属 DESKTOP |
| x86_64 | 通过 | 未验证 | 构建通过；桌面实例的运行证据属 DESKTOP |
| loongarch64 | 通过 | 未验证 | 构建通过；本机无实板 |
| riscv32 | 失败（内核）/ 通过（用户态） | N/A | `kernel/mm/cow.c`、`fault.c`、`madvise.c` 在 32 位下指针宽度不匹配，`check-riscv32-bringup` 编译失败；见 [MM-05](../gaps/mm.md) |
| arm32 | 失败（内核）/ 通过（用户态） | N/A | 容器内 `check-arm32-bringup` 报同一类 32 位 MM 问题与 `elf_rust ABI drift` 静态断言；见 [MM-05](../gaps/mm.md) |
| armv7m (STM32F103) | 失败 | N/A | `make stm32f103-bringup` 在 `tools/rust-kernel.mk` 的解析期断言处停下；`check-stm32f103` 另报硬编码 MMIO 地址。见 [DRIVER-09/10](../gaps/drivers.md) |

构建矩阵此前会在**任何**架构上直接失败：`mm/mremap.c` 在 profile 里而 `RUST_DONTUNMAP_TARGET`
只对 x86_64/aarch64/riscv64 定义，make 在解析期就报错。本轮补齐 riscv32/loongarch64/arm32
的 Rust 目标并逐个实测其对象可编译（见 §5 的"已修"）。

## 4. SMP 门禁

`smoke-sched-stress-smp2` 与 `smoke-proc-stress-smp2` 是本轮新增的两个门禁：调度器
runqueue 放置/迁移/跨 CPU 唤醒与进程退出、wait、线程组回收此前只有 `NR_CPUS=1` 的门禁，
而既有的 SMP 变体只覆盖 vfs/futex/swap。两者都：

- 与单核 target 跑同一负载，但各自独立构建（`NR_CPUS=2`）、独立日志、独立结果；
- 断言启动日志出现 `[SMP] 2/2 configured CPUs online`——没有这条，接线错误会让变体在
  `-smp 2` 下启动单核镜像并照样打印自己的 PASS 标记；
- 不改变原单核 target 的语义。

四个 target 在本候选上都通过（`smoke-sched-stress`、`smoke-sched-stress-smp2`、
`smoke-proc-stress`、`smoke-proc-stress-smp2`），单核与 SMP 结果分别保留在各自的日志里。
两个 SMP2 变体已加入 `ci.yml` 的 smoke job，与既有的 vfs/futex/swap SMP 变体并列。

## 5. `check-a20-tests` 基线

在干净树上跑 `make check-a20-tests`（476 个用例，stdlib unittest）。分类规则：
**已修**＝本轮修好且当前通过；**既有**＝在本轮基线提交上就失败，与本轮改动无关；
**环境**＝失败由运行环境而非源码状态决定。

本轮已修（当前通过）：

- `check-a20-tests` 缺 `user/svc/a20_services_idl.h` 前置：该头是生成且被 gitignore 的，
  门禁只在"恰好被别的目标先生成过"的树上通过，在干净 checkout 上必红——CI 的
  `toolchain-gates` job 正好只跑它，所以这是 CI 红的一个直接原因。
- `check-smoke-cases` 报 `smoke-elf-exec-aarch64` 无 make 目标：用例一直在表里，目标缺失，
  于是 AArch64 exec 路径根本没有门禁。
- 构建目录被哈希后，`test_smoke_build_paths`、`test_a20` 的 3 个用例仍在断言描述性目录名；
  改为断言真正的不变量（每个 override 都到达 make 且选出不同目录）。
- `tools/rust-kernel.mk` 缺 riscv32/loongarch64/arm32 的 Rust DONTUNMAP 目标：任何带
  `mm/mremap.c` 的 profile 在这三个架构上 make 解析期即失败，也是 riscv32 opt-in peer
  用例不可能通过的原因。三者对象已实测可编译。
- `tools/tests/test_rust_user_make.py` 用 `ROOT/.git/...` 定位共享构建锁：linked worktree
  里 `.git` 是文件，整个模块在 worktree 里以 `NotADirectoryError` 报错——而这正是本仓库
  规定的每包工作方式。改为经 `git rev-parse --git-common-dir` 定位。
- `tools/tests/test_stamps_clean.py` 构造的 Namespace 缺 `syscall_smoke_task_construction_test`，
  `tools/stamps.py` 已经要读它，5 个用例直接 `AttributeError`。

既有失败（在本轮基线提交上就存在；当前 8 个 error + 1 个 skip）：

| 用例 | 根因 | 归属 |
| --- | --- | --- |
| `test_vma_region_host`、`test_rust_merge_policy_against_actual_c_function` | 在源码里找不到自己绑定的标记，`ValueError: substring not found` | MM |
| `test_tcp_send_backpressure` | 同上 | NET |
| `test_usercopy_strings_host`、`test_usercopy_nommu_host`、`test_tlb_transaction_races`（2 个）、`test_process_vm_host` | 把内核 C 抽出来用宿主 gcc 编译，但抽取窗口不含新增的 `extern` 声明/Rust 桥接原型，或 C/Rust 接口签名已变 | MM |
| `test_production_module_partial_results_and_references` | 同上（抽取/链接） | PROC |

CORE 包此后修好了本表原本的 `test_dispatch_ownership`（负控的重排锚点随 `sched_runq_steal_locked`
改写而失效，导致双重解锁），并新增 `check-wait4-controller` 把只被 `tools/tests/` 通配捡起、
却从无人运行的 `test_wait4_host.py` 接进 `host-tests`；见 [CORE-11](../gaps/core.md)。

环境类：1 个 skip（缺可选宿主依赖）。另有一类只在非 C locale 下出现的失败——宿主 `make`
输出中文报错文本时，断言英文 `No rule to make target` 的用例会失败；这类失败与源码状态无关，
本页的计数在 C locale 下取得。

同一棵树上的宿主侧门禁（CI `toolchain-gates` job 的成员）也并非全绿，见 §6。

## 6. CI 工作流的当前状态

`.github/workflows/ci.yml`、`native.yml`、`release.yml` 的逐 job 状态。**"本地复现"指在
同一固定构建容器或宿主上按 job 的 `run:` 步骤逐条执行；`cloud` 指在配置好的云端 CI 上
实际派发。**

| job | 内容 | 当前状态 |
| --- | --- | --- |
| `buildenv-image` | 解析小写镜像名 | 纯 shell，无失败面 |
| `toolchain-gates` | 宿主侧源码契约门禁（裸 runner，24 项） | **本地复现：红**。CORE 包修好 `check-arch-boundary`、`check-task-lifetime-boundary` 与 `host-tests` 的 proc 成员后，仍为红的是：`check-a20-tests`（§5，剩余 8 个用例属 MM/NET/PROC）、`check-honesty-policy`（lwIP accept 阶段接线断言）、`host-tests`（约二十个 harness 需要各自的 Rust 对象或 per-arch include，见 [CORE-11](../gaps/core.md)）、`check-abi-boundary` 与 `check-envelope-coverage`（`a20_envelope_acquire_race_test` 未登记，见 [ABI-09](../gaps/abi-ipc.md)）、`check-task-state-boundary`（见 [ABI-10](../gaps/abi-ipc.md)）、`check-doc-drift`（`v1.0-requirements.json` 需重新生成；同目标的 `tools/check_doc_citations.py` 仍报越界引用，见 [DOCS-05](../gaps/docs.md)；另有 `gen_linux_syscall_coverage.py --check` 因 ABI-09 失败） |
| `ci-kernel-arches` | 解析内核构建矩阵 | 纯 make，正常 |
| `cluster-runtime` | 集群运行期门禁 | 未本地复现（需容器 + 60 分钟级 QEMU） |
| `build-{riscv64,x86_64,aarch64,loongarch64}` | dev-build + `pkg-repo` + `image-world` | 未本地复现（需容器 + 磁盘预算）；riscv64 的 dev-build 已在 §2 单独验证 |
| `kernel-build-{6 架构}` | 逐架构 bring-up 构建 | 本地复现见 §3（arm32/armv7m 在容器内） |
| `native`（`native.yml`） | Native 程序 + 共享断言（riscv64/aarch64） | 未本地复现（需容器） |
| `smoke-riscv64` | 60 分钟级 QEMU 冒烟套件 | 未本地复现；其中 riscv64 shell、arch/MMU 矩阵与两个 SMP2 压力门禁已在 §2/§3/§4 单独验证 |

被跳过的步骤及其分类（skipped 不计入覆盖）：

| 步骤 | 跳过条件 | 分类 |
| --- | --- | --- |
| `cluster-runtime` 的"扩展混合架构与 MCU 验收" | `github.event_name == 'workflow_dispatch'` | 手动触发专属；push/PR 上不覆盖 |
| `build-*` 的 "x86 kernel driver FP-state regression" | `matrix.arch == 'x86_64'` | 架构专属；只在 x86_64 上覆盖 |
| `native` 的 "AArch64 Linux ABI pressure" | `matrix.arch == 'aarch64'` | 架构专属；只在 aarch64 上覆盖 |
| `native` 里 riscv64 的硬件私有 RTC/UBD 用例 | runner 自报平台排除 | 硬件专属；不计入覆盖 |

`release.yml` 是 tag 触发，不在候选验收路径内；它的运行要求与 §2 相同的重建输入。

**云端派发本身没有发生**：它需要把候选推到远端，而本工作包的规则禁止 push。因此本页的
每一条 CI 结论都来自本地复现，不是云端运行结果。
