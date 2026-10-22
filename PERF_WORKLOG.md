# A20OS 性能改善工作记录

- 用户授权：独立 worktree 分析与改善，持续到 2026-10-23 00:00 Asia/Shanghai（2026-10-22 16:00 UTC）。复杂逻辑可按收益使用 Rust，简单逻辑不重写。
- 工作目录：`<worktree>`，起点 `b5ce67024cff`。原 checkout 保持不变。
- heartbeat：`a20os`，每 10 分钟恢复本线程，截止上述 UTC 时间。恢复时先检查时钟、git diff 和现有构建/QEMU/测试进程，避免重复实验。
- 原始基线与分析：`docs/status/measurements/{apk-linux,bottlenecks}-2026-10-22.md`。
- 原内核、FAT 和 APK world 使用原 checkout 中只读基线；runner 每轮复制 world，QEMU 使用 snapshot。`build/benchmark-host-root` 是指向原 checkout 的只读使用 symlink。
- 约束：不 push/merge/deploy。编译与性能测量串行；两侧 affinity 4,6，KVM 2 vCPU/2 GiB，三次重复、交替 Linux/A20；正式速度使用关闭诊断计数的内核。
- 用户后续偏好：通用 C 改动只做一种架构的验证，当前选 x86_64。除非实际改动涉及架构接口/ABI，不再展开多架构构建。
- 用户新方向：对测量证明有热点的线性查询/有序插入考虑堆、平衡树等；提供快版/嵌入式省内存版仅在实际差额达到 KiB 量级、且嵌入式仍保留该模块时；若差额很小或模块本来被裁剪则不增加双实现。

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

## 2026-10-22 20:13 北京时间

- 七项 APK 复测完成：42/42 ok，输入哈希不变，摘要在 manifest 内（不是单独 summary.json）。CPU/内存/共享锁基本接近前轮，但 fork A20 325.37、Linux 3081.92 ops/s 都低于早期；已启动当前时段原内核同参数完整42样本对照，避免忽略潜在回退。
- **当前运行**：`bench-apk-linux.py` 原内核对照，证据 `build/evidence/perf-stock-apk-current`，日志 `build/performance/apk-stock-current.log`。期间禁止编译/并行性能任务。
- yield keep-current 候选已写入 `kernel/proc/sched.c`，保留两次独立重检，RR重置移入现有锁区间，非RR不额外采时。已构建保存 `build/performance/yield-keep.elf`，尚未做 guest 回归或性能测量，未提交。
- 新 `tools/tests/test_sched_keep_current.py` 执行真实 sched/helper/双任务锁函数，涵盖 IRQ on/off、普通/queued/RR/blocked/stopped/migrating/throttled/peer dispatch，以及在第一次拒绝后精确注入 late wake，已通过。
- 旧 `test_sched_yield_accounting.py` fixture 落后于 HEAD 的 EEVDF Rust 数学接口和 deadline lock，原本不能编译。已更新 fixture 声明、字段并链接真实 eevdf_math.rs；原正向测试与 legacy负控制通过（负控制打印 incorrect accounting 是预期）。current migration 测试通过。
- 之后：原内核42样本完成后先解释 fork 绝对值差异；随后用 `sched-regression-config.json` 注入原 checkout 的 sched_stress/futex_stress/gs_base_test 做 x86_64 SMP2 guest 回归，再用 `yield-config.json` 对 table-hint/yield-keep AB/BA/AB，关闭计数器。

## 2026-10-22 20:18 北京时间：数据结构筛查（尚未改动）

- 已有高级结构：EEVDF ready 队列 Treap，wait/alarm timer 最小堆，PID hash，VMA 有序索引。不要盲目替换语义适合FIFO的队列。
- 待测候选：`page_cache_state.rs:a20_pagecache_dirty_insert` 的每 vnode dirty 有序双链表。头尾插入已 O(1)，中间随机首次弄脏需要线性扫描。应先用**相同缓存规模、预先读取使页面驻留、定序/逆序/随机首次pwrite**对照，看随机规模增长是否暴露 O(n²)，再决定平衡树。可用静态同 ELF micro、host/guest 文件路径明确设在普通文件系统而非tmpfs。page_cache 在 MCU trim 中不存在，若改这里不必给裁剪的 MCU 额外双实现。
- 省内存候选：`wait_timer.rs` 的 STORAGE_CAPACITY 固定8192，而C回调 WAIT_TIMER_HEAP_MAX 可为64/4。Rust等待heap对象在 MCU timer_heap.c 存在时无条件链接；MCU启动 proc_init→proc_sched_runq_init→proc_timer_heap_init→Rust init，源码看不出可被裁剪。alarm Rust已有非MCU guard、MCU用小Cheap，不能误报alarm同样问题。应使用 nm/size 确认 wait heap 对象/最终镜像实际占用后，考虑构建时容量贯通（无需另写一个低级算法）。该模块 heap初始化在boot调用，不应仅以链接了obj就宣称存活。
- Rust构建flags stamp当前未包含 WAIT_TIMER_HEAP_MAX；若做容量编译配置，需保证 C/Rust 单一有效容量、合法范围、flags stamp和增量重建；保留默认8192和host tests小容量覆盖。STM32_QEMU 在Makefile2111另行 CFLAGS 强制4，要统一处理而非只看用户变量。

## 2026-10-22 20:21 北京时间

- 原内核当前完整 APK 对照42/42 ok，已归档。当前A20 fork原259.38→MM改进325.37 ops/s（+25.44%）；早期586绝对值当前原内核也不能复现，Linux也明显变慢。完整表与范围已追加报告。
- **未解决关注点**：完整套件 SysV semaphore switch 原207037→MM改进192483 ops/s（-7.03%），guest范围不重叠。不能宣称所有负载无退化；应后续用 stock/table-hint/yield-keep 定向交替复查，而非用宿主波动笼统掩盖。
- yield候选首次guest sched_stress/futex_stress通过；GS_BASE失败是测试安装到/usr/bin/gs-base-test，但程序内部exec硬编码/bin/gs_base_test。初次失败证据保留 `perf-yield-keep-regression`，不能计入全通过。已改安装路径重跑，配置 `sched-regression-config-v2.json`。
- **正在运行** exec session **48858**，日志 `build/performance/yield-campaign.log`：先v2三项guest回归（正确GS路径），成功后自动 table-hint/yield-keep 三轮AB/BA/AB，配置yield-config-v2.json（micro getpid、yield 100万、1t/2t私锁/2t共享锁、SysV switch），每项3秒或固定次数，关闭计数器。各轮目录 `perf-yield-paired-{1,2,3}-{table-hint,yield-keep}`。期间不要编译或并行性能任务。
- 省内存 waitheap 确认源码startup调用，但尚未nm最终验证。MCU alarm已小C版，不动它。暂未写新数据结构或容量代码，先完成上述yield候选。

## 2026-10-22 20:26 北京时间

- yield campaign session48858完成；v2三项guest全部通过，72性能样本全部0/基础镜像不变，已归档 `yield-keep-2026-10-22*`、追加报告第5节。
- A20 medians：yield1681.08→1557.69ns (-7.34%)，1t yield +7.75%，2t私锁+6.77%，共享锁3326.33→3932.81events/s (+18.23%)，SysV203844.68→271330.37 (+33.11%)；三对方向一致；getpid615.16→616.69ns控制基本不变。准备提交该候选。
- 用户的小内存建议已落到下一项候选：`wait_timer.rs` 编译期读取容量env（合法1..8192，默认8192），Make从最终CFLAGS派生实际容量并写入Rust flags stamp，避免MCU运行limit小却保留8192数组。未改算法。新host tests在4/64/8192容量运行相同ownership/取消/过期/竞态回归，并以nm验证真实BSS缩小；非法0/8193/nondecimal拒绝，2项测试通过。
- **当前构建** exec session **68128**，输出 `build/performance/timer-capacity-build.log`：同一x86_64 BUILD_DIR依次cap4/64/8192构建Rust对象，记录nm、stamp和mtime到 `timer-capacity-sizes.json`，检查增量重建。旧8192对象保存 `build/performance/wait-timer-before.o`。此构建尚未最终审查，容量候选未提交。
- 下一步核对容量对象实际BSS、与旧对象默认导出代码一致性；默认x86_64内核构建/guest计时回归；配置选择只用一架构多容量，不展开架构矩阵。之后原内核/yield-keep 做SysV定向对照并处理dirty索引测量。

## 2026-10-22 20:32 北京时间

- yield优化已提交 `87081c92`（含主机回归修复、72样本、原七项的84样本当时段对照报告）。
- Waitheap构建容量4/64/8192对象的HEAP实测128/1568/196640B，stamp包含容量，4→64→8192均重编，重复8192无重编。旧与新默认8192五个导出函数.ltext字节完全一致，记录 `timer-default-code-check.json`。
- 默认新x86 kernel `timer-default.elf` guest三项sched/futex/GS全部通过。
- cap64完整kernel `timer-small64.elf` guest sched/GS通过；futex失败 `wake-op-batch-incomplete errno=11`，源测试固定70个带超时waiter，超出64真实限制。**不能把此case标绿**；是配置容量语义，源码与host fixture证明limit在pin之前检查；不为测试扩大64容量。原始失败证据保留 `perf-timer-small64-regression`。
- 新加小配置过期临时数组也按 `min(storage_capacity,128)` 缩小：原4项profile仍分配128个Expired（x86对象expire栈扣0x828），没有必要。默认8192仍128不变。
- **当前正在执行**：host容量v2回归→同x86 cap128 full kernel构建→三项guest回归，输出 `timer-small128-build.log` / `timer-small128-guest.log`，证据 `perf-timer-small128-regression`。capacity source尚未提交。等结束后重跑4/64/128/8192对象记录BSS/栈与默认代码一致性，整理报告后提交。
- 下一项仍是原内核/yield版的SysV定向对照，以及用户建议的dirty有序索引规模测试。截止北京时间午夜不变。

## 2026-10-22 20:37 北京时间

- 容量候选最终对象v2 cap1/4/64/128/8192：HEAP56/128/1568/3104/196640B，expiry局部栈24/104/1064/2088/2088B。实测JSON和完整证据（含64容量不足失败）已归档timer-capacity-2026-10-22*、报告第6节。
- host最终fixture在1/4/64/8192检查单项和满4项批次、引用/取消/过期竞态等，通过；非法容量拒绝编译。默认五个函数代码字节与旧对象一致。
- cap128最终kernel三项SCHED/FUTEX/GS通过（70 waiter能容纳）；cap64最终版（含栈批次缩减）SCHED/GS通过，task_lifetime实测capacity64、full_failures0、heap_violations0、state_violations0。仅完整futex70waiter负载因容量64明确失败，未改配置掩盖。
- 还需最终默认内核重建+hash核对（预期仍与yield-keep f82e...一致）；然后提交容量修复。
- 当前所有对象构建和guest已结束。准备下一段stock/yield-keep SysV+fork短负载4轮AB/BA/AB/BA交替，避免因全套两批顺序结果忽略7% SysV疑问。之后按数据结构筛查计划测dirty索引。用户最新思路与测试架构偏好在本日志顶部已固化。

## 2026-10-22 20:39 北京时间：恢复工作入口

- 容量修复已提交 `e1fea20a`；默认最终内核重建哈希与yield-keep完全相同 `f82e173...`（完整hash见前文），不存在标准版代码/镜像变化。工作树代码已提交。
- **当前正在运行** exec session **94980（若会话号不可用，以ps/log为准）**：四轮 stock/yield-keep AB/BA/AB/BA，配置 `build/performance/stock-yield-focus.json`（getpid200k控制、SysV switch5秒、fork5秒），关闭计数器，同APK/镜像，Linux/A20串行。日志 **`build/performance/stock-yield-focus.log`**，证据 **`build/evidence/perf-stock-yield-focus-{1,2,3,4}-{stock,yield-keep}`**，目标48样本。完成前不要编译/并行性能任务。
- 首先检查上述进程和manifest是否齐全，完成后解释前述7% SysV疑问并归档结果；`stock.elf` 为原cab388内核只读副本，yield-keep为f82默认新内核。
- 后续进入用户数据结构建议：测 `page_cache_state.rs` 中 per-vnode dirty有序双链表**中间插入**。头尾已有O(1)，所以必须区分定序/逆序/随机，不把所有链表都改树。建议先预读取驻留同规模普通文件页，再计时每页首次pwrite1字节，随机种子固定，以1024/4096/16384页等规模比较；时钟只放循环两端，同静态musl ELF在host/guest运行，文件放host工作目录/guest /root而非可能tmpfs的/tmp；必须包括清理。该文件模块在MCU裁剪，所以无需为其加入MCU双实现。先测得规模热点再改为平衡树/索引。
- 若需编译静态musl辅助：原 checkout user/build/x86_64/.musl/*/lib/libc.a 有已建musl1.2.6；可用cc -nostdinc -I该目录obj/include -I user/external/musl/include -I...arch/x86_64 -I...arch/generic 编对象，cc -static -nostdlib -no-pie +crt1.o/crti.o/对象/libc.a/-lgcc/crtn.o。旧micro-v2保持不变；新数据结构fixture另命名ELF，不能污染前轮哈希。
- 仍可考虑修复诊断perf缓冲截断，或真正COW批量克隆，但以新数据结构测量为下一优先。直到北京时间午夜持续推进，不push/merge/deploy，不再要求用户重复授权。

## 2026-10-22 20:53 北京时间：恢复入口

- stock/yield 四对定向复查完成48/48：SysV223496→264394 ops/s，中位+18.30%，四对同向；fork382.76→586.18但范围大，不能推广精确收益。报告第7节和完整gzip/CSV/JSON已保存，尚未提交。
- dirty首次试验因/root目录不存在失败（原证据保留）；配置v2用/var/a20-dirty-index.data，初探及正式3轮54样本成功，fs_type=ext4、verified=1、base_unchanged。三轮A20随机每页1024约5.2us、4096约9.6us、16384约34.8us，顺/逆序约4.2–4.9us；符合链表中间插入线性扫描。Linux第三轮出现明显系统干扰（13–27us），保留而非删除。原checkout独立GTK QEMU和其它worktree测试进程属于用户其它工作，不杀进程，复测采用交替内核并记录。
- 已写候选：保留有序dirty链表和global列表，新增intrusive RB索引仅替换中间查找，避免回写迭代语义变化。每page额外24B（LP64），每vnode8B；pagecache在MCU整体裁剪，不增加双实现。Rust现有page_cache_state本来拥有结构状态，不是为简单逻辑另行重写。
- host v1通过18 Rust tests（含16384正/逆/交错插入、3万次随机增删、blackheight/红边/parent/有序链表成员检查）和20万次旧C链表/Rust对照；回写、truncate、pin/refcount语义等通过。输出build/evidence/dirty-rb-host-v1。只编x86目标。候选未提交。
- **当前正在构建** dirty-rb内核，日志build/performance/dirty-rb-build.log；等完成复制到build/performance/dirty-rb.elf，再先运行fs/pagecache/mmap相关guest回归，之后yield-keep/dirty-rb AB/BA/AB三轮同dirty-config-v2对照。构建与性能实验不得重叠。

- 20:55补充：dirty-rb构建完成，ELF4742cf56f4e055ffa1e75850fe6f5a87f61221bfa8566275541a7dfe400ba14b，保存在build/performance/dirty-rb.elf。**当前exec session82260**正在跑mm_stress、vfs_concurrency、FSYNC_TEST_DIR=/var的fsync_durability_test，证据perf-dirty-rb-regression，日志dirty-rb-regression.log。回归未完成前不启动同参数对照，候选仍未提交。
