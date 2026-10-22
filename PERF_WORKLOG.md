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
