# A20OS 性能改善工作记录

- 用户授权：独立 worktree 分析与改善，持续到 2026-10-23 00:00 Asia/Shanghai（2026-10-22 16:00 UTC）。复杂逻辑可按收益使用 Rust，简单逻辑不重写。
- 工作目录：`<worktree>`，起点 `b5ce67024cff`。原 checkout 保持不变。
- heartbeat：`a20os`，每 10 分钟恢复本线程，截止上述 UTC 时间。恢复时先检查时钟、git diff 和现有构建/QEMU/测试进程，避免重复实验。
- 原始基线与分析：`docs/status/measurements/{apk-linux,bottlenecks}-2026-10-22.md`。
- 原内核、FAT 和 APK world 使用原 checkout 中只读基线；runner 每轮复制 world，QEMU 使用 snapshot。`build/benchmark-host-root` 是指向原 checkout 的只读使用 symlink。
- 约束：不 push/merge/deploy。编译与性能测量串行；两侧 affinity 4,6，KVM 2 vCPU/2 GiB，三次重复、交替 Linux/A20；正式速度使用关闭诊断计数的内核。

## 2026-10-22 19:50 北京时间

已导入前轮工具、报告与证据，内核逻辑尚未修改。第一项：`free_vma_pages()` 利用 `pt_lookup_leaf()` 已返回的缺失子树范围跳过稀疏空洞。后续依次考虑 COW 克隆重复查找和 yield 路径；每项先正确性回归再同参数对照。

当前没有运行中的本 worktree 构建或性能实验。

## 2026-10-22 19:54 北京时间

- 已创建本地分支 `perf/a20-performance`，前置 benchmark/诊断工具与证据提交 `228d5cb0`。
- 第一项修改完成：`kernel/mm/vma.c` 的缺失子树跳过；内核保存在 `build/performance/sparse-skip.elf`，源构建目录采用与基线相同 identity。
- 原/新内核各 30 样本全部通过：64 MiB 稀疏 fork 0.4603→0.1664 ms，256 MiB 1.3847→0.1678 ms；已触碰页面尚未改善。报告与 CSV/JSON 已生成在 `docs/status/measurements/performance-improvements-2026-10-22*`。
- 主机回归、修复内核完整 mm_stress/swap_test 通过。swap_test 有 ext4 orphan cleanup -95 警告，正在用原内核相同回归判断是否既有行为。
- 当前原内核回归进程：exec session 26092；输出 `build/performance/mm-regression-stock.log`，证据 `build/evidence/perf-stock-mm-regression`。先确认完成再启动新的性能测量。
- 下一步：归档 guest 回归，提交第一个修复。COW 路径可以先省掉 Rust clone_leaf 中只用于 private-copy 分支的重复 probe，再考虑在 snapshot 中携带已锁定 table/slot，避免 downgrade/unlock 再从根查找。保留父节点锁内 snapshot+pin、失败引用回滚和大页 status level 语义。每个实验保留单独 ELF/证据，逐项衡量。

## 2026-10-22 19:55 北京时间

原内核同参数 mm_stress/swap_test 均通过，ext4 -95 告警也复现。原/新两份回归 manifest 已并入报告的压缩证据。当前无运行实验，准备提交稀疏修复，然后验证 Rust clone_leaf 重复 probe 消除。

## 2026-10-22 19:56 北京时间

- 稀疏修复已提交 `63b56120`，保留 `sparse-skip.elf` 不覆盖。
- COW 第二项试验已修改 `cow_clone.rs`：把仅 private-copy 路径使用的 unlocked probe 移入该分支；share 路径以既有锁内 snapshot 验证 presence/type。COW clone/fault/status 主机回归 46/40/41 checks 通过，内核已构建，保存 `build/performance/cow-probe.elf`。
- 正在执行 **交替内核** 三轮对照，exec session **65019**；日志 `build/performance/cow-paired.log`；每轮 kernel 顺序 AB/BA/AB，每次内部 Linux/A20 串行；fork、16MiB touched fork（200 次）、stress-ng fork 3s。输出 `build/evidence/perf-cow-paired-{1,2,3}-{sparse-skip,cow-probe}`。
- 此实验完成前不要启动编译或其它 QEMU/性能任务。需要验证收益后才保留该实验修改，尚未提交。

## 2026-10-22 20:00 北京时间

- COW probe 试验完成 36 样本，全部成功但无一致改善：fork 166579→168169ns，touched 6645709→7371317ns，stress-fork 232.92→243.52 ops/s 且波动大。已归档六份 manifest 到报告 gzip，保存 `tools/bottleneck-cow-probe.patch`，正式 Rust/host test 已恢复到 HEAD。
- 正在构建第二个候选：`pt_table_empty()` 优先检查刚清除项的邻居；仅提前返回非空，最终空表仍完整扫描，保留 swap 语义。用于减少连续驻留页回收时每次重复扫描整个已清空前缀。
- 新 host fixture `test_pt_table_empty.py` 对两种页表几何、有/无 CONFIG_SWAP、每个剩余叶位置和 hint、随机稀疏及正反序清空检查真实函数对照完整扫描，通过。稀疏 teardown 回归也通过。
- 当前 build exec session **82074**，日志 `build/performance/table-hint-build.log`。等构建结束保存 `table-hint.elf`，以同一个 `cow-config.json` 对照 `sparse-skip.elf` AB/BA/AB，期间不要编译。候选未提交。

## 2026-10-22 20:05 北京时间

- 判空优化已完成三轮 36 样本，全部成功，已归档到报告及 gzip/paired CSV/JSON。最小 fork 中位 0.1752→0.1625ms，touched 6.9165→5.9422ms；stress fork 242.50→285.15 ops/s。默认 stress fork 波动大，但每对同轮 +17.1/+23.7/+29.0%，touched 每对同向下降。保持范围和不确定性，没有推广所有 fork。
- `table-hint.elf` sha256 `8668a54dd95342733047584914a801c911b9b9174fb616af38131d08f458c199`，包含稀疏跳过+判空邻居检查，不含 COW probe 试验。
- 新内核完整 mm_stress/swap_test 通过；AArch64、RISC-V64 与 x86_64 CONFIG_SWAP=n 的 mm.o/vma.o 编译通过；主机真实判空、稀疏回收、TLB races、锁顺序通过。当前所有这些进程已结束。
- 下一步先对当前内核重跑原七项 APK benchmark（5秒×3轮，两侧42样本），再考虑调度路径。计数关闭，构建不可与性能测量重叠。
- yield 的低风险候选：`sched()` keep_current 和 late_ready 分支都先解开 park_lock，再重新获取同一锁调用 `rt_rr_dispatch_task(cur,timer_get_ticks())`，即使普通 EEVDF 任务也先求时间。可把 RR 检查/重置放进已有锁区间，仅 RR 任务采样时间。可以抽同一 helper 复用两处分支，但必须保留 W2 再检查、迁移/RT throttled 条件、on_rq 移除、IRQ状态和 park token；真实函数 seam 测试+SMP guest 压力后再衡量单线程 yield/原共享锁负载。
- 不要贸然启用 x86 FSGSBASE 来减少 MSR：当前 GS/NMI 窗口检测依赖用户不能通过 WRGSBASE 写高位内核地址；CR4.FSGSBASE 会开放用户指令，必须先处理该安全不变量。暂不实施。

## 当前正在进行：原七项 APK 回归

- 判空优化已提交 `0886af84`。第一项提交 `63b56120`，前置基准提交 `228d5cb0`。
- 已启动 `python3 tools/bench-apk-linux.py --build-dir build/performance/apk-input --image <repo>/build/images/benchmark-x86_64.img --out build/evidence/perf-mm-improved-apk --cpus 4,6 --seconds 5 --runs 3 --timeout 40`。
- 输出 `build/performance/apk-mm-improved.log`，证据 `build/evidence/perf-mm-improved-apk`，预计数分钟。先检查该进程/manifest 是否完成，**未完成前不要编译或并行跑性能测量**。
- `apk-input` 中 ELF symlink 指向已保存的 table-hint，FAT 指向原基线（只读使用），镜像由 runner 私有复制。
- 下一次 heartbeat：等该阶段完成，核对42样本/指标/哈希；归档报告中相同 Linux 版本与新 A20 的数据，明确不能忽略宿主波动；之后推进上述 yield 候选。今夜午夜的截止约束仍有效，未完成部分如实记录，不需用户再次授权。
