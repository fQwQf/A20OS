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

## 2026-10-22 21:00 北京时间：dirty RB v2 正在复测

- v1完整三对108样本全部成功：随机16384页中位32264→4693ns/page (-85.45%)，4096页8527→4387 (-48.55%)；但顺序1024/4096页+7.8%/+7.3%，不能忽略。原始v1 ELF、gitdiff和host日志已保存，尚未提交候选。
- v2给严格高于头/低于尾的插入直接使用头尾叶子空child，不再从root搜索；中间插入仍RB O(log n)，writeback链表不动。额外host覆盖旧descriptor改index复用、重复keys及0/u64max；18tests+20万Cdiff通过。
- 实际相同x86 C编译参数 sizeof page_cache_page_t 336→360、vnode_t296→304，记录build/performance/dirty-rb-layout/sizes.json。每2048初始descriptor额外48KiB、262144最大容量额外6MiB（对应1GiB数据缓存的0.59%）；按需池增长，不一次分配全部。MCU整体无pagecache，不加无用双实现。
- **当前exec session93437**，日志build/performance/dirty-rb-v2-campaign.log：v2 guest mm/vfs-concurrency/fsync后自动旧yield-keep与dirty-rb-v2 AB/BA/AB三轮共108性能样本。目录perf-dirty-rb-v2-regression / perf-dirty-rb-v2-paired-*。不要在此阶段编译或并行性能实验。v2 ELF da7c3d85e2e9e23abfb1dab4ad86b29846d4c20ae2cccfcd4f9cfae5716bd085。
- 等结束比较所有顺序、逆序、随机规模及ranges，收益确立后归档完整raw/gzip/CSV/JSON与源码、提交候选。若顺序仍小回退要明确报告权衡，不掩盖。

## 2026-10-22 21:04 北京时间

- dirty RB v2正式三对108样本完成并归档 dirty-index-2026-10-22*：随机1024/4096/16384页延迟-16.33%/-47.88%/-84.51%，三对均同向；大规模顺/逆序约持平。小规模逆序+4.25%/+6.08%已明确报告。
- 额外fsync三对108样本全部成功，归档dirty-flush-2026-10-22*。随机16384页的fsync本身+7.01%，但写+fsync两段计时之和649.993→231.798ms(-64.34%)，三对均下降；4096随机合计-24.61%；顺/逆序合计-1.49%～+2.12%。校验、准备、close/unlink不在这两段内，不冒称端到端。
- 18项host/20万差分及3项guest正确性已完成，候选准备提交。所有性能/构建进程已结束。下一项考虑brk/stack克隆range逐页扫描：mmap已经present walker，但mm_fork_clone_range仍逐地址；需保持DONTFORK/WIPEONFORK、dst already present和huge base语义。先新micro测untouched brk规模，不复用已冻结旧micro。

## 2026-10-22 21:10 北京时间：brk/stack 稀疏克隆候选

- dirty RB 已提交e31aa45c，回退点和负面小规模/回写代价均在报告第8节。
- 新brk micro先grow原始brk、可选每页触碰，计时100次fork/wait；touched子进程写第一页并检查父COW隔离，最后核对所有父页和恢复brk。同静态ELF初探8/8通过；原64MiB untouched约1.080ms/fork、256MiB3.742ms、空堆0.160ms。
- C候选复用present页表walker给brk/stack range，checked模式保留原Rust per-address政策、huge base、child-present、swap和失败语义；mmap原unchecked模式不改。无Rust重写、无ABI更改。
- 新真实C walker fixture：20万随机range×swap开关，比较旧逐地址循环；同时比较原present walker语义。涵盖holes、policy、dst present、huge部分边界、swap EEXIST/EAGAIN、失败与部分进度。UBSan主机通过，原Rust COW clone45checks通过。构建x86_64完成，ELF f3896df26b08ecf2152f7623ef0a7736f2ddf18ec4635fd9f3642eea23c82b01 保存brk-range.elf。
- **当前exec session9947**，日志build/performance/brk-range-campaign.log：先guest mm_stress、process-vm-exec-cow、TLB teardown、swap_test；mm_stress正确安装/bin/mm_stress以满足self-exec。成功后自动dirty-rb-v2/brk-range AB/BA/AB三对，4个brk规模+原fork/touched/stress+getpid控制，共96性能样本，关闭计数器。输出perf-brk-range-regression及perf-brk-paired-*。期间不编译或并行性能实验。
- 候选和brk工具/fixture尚未提交，等同参数复测和dense/no-brk控制结果。

## 2026-10-22 21:17 北京时间

- brk候选全部完成：4 guest回归通过，96性能样本成功。sparse64/256MiB brk中位1.0646→0.1626ms /3.7943→0.1645ms，-84.73%/-95.67%，各三对一致；已触碰16MiB brk+0.53%。其它fork负载方向混合，已报告而不推广。完整brk-range-2026-10-22*已归档。注意stress-ng行解析应取倒数第二列real ops/s，非倒数第三列sys time，最终已核对修正为274.27→289.18ops/s，但三对不一致。
- 测量协议新发现：同内核getpid控制在stress之前618.79ns，之后1303.49，写perf0后946.86；原始目录perf-counter-lifecycle-initial已保留。源码确认procfs_meta_create对perf用4096栈buffer生成内容，可能在stat/lookup启用收集；--no-counters只是runner不主动采样，不能保证全程disabled。下一项修正诊断快照截断/metadata副作用与runner明确disable，并用before/after小fixture核对。
- 当前所有本任务构建/性能进程已结束，brk候选准备提交。不得推送/合并/部署；截止午夜仍有效。

## 2026-10-22 21:27 北京时间：诊断快照候选

- 新候选未提交：perf/lock content plan使用按需grow generator，4KiB起、饱和则倍增到最多1MiB，无法完整输出时返回错误而不交付截断；metadata stat长度0且不调用formatter；write-only打开不生成快照。
- perf格式首行新增collection_enabled_before_snapshot，首次完整快照结束才arm，扩容重试不提前arm；原已启用状态不变。顺带修正syscall start=0（关闭时入口）却在本次调用内enable后，把boot uptime算作该syscall elapsed的错误。
- host真实C代码ASan/UBSan：全部1024个sysprof+所有kernelcounter=UINT64_MAX时完整85,826字节，峰值分配128KiB、6次尝试；饱和边界、1MiB限额、每轮ENOMEM/format error清理、zero-start profile guard均通过。Rust procfs完整单测及20万Cdiff通过，只编x86。构建完成procfs-diag.elf af9789c1929074aacb73743e833c95b287508901bed408ced1e7d44d9970f163。
- 两个runner已改：正式测项前明确echo0关闭、测后取状态首行并关闭；老内核无状态首行记录None。analyzer新增--preserve-counter-state仅供生命周期负控制，manifest明确policy，不再把no-counters等同所有程序全程未启用。尚未提交、待guest真实验证。
- **当前exec session85503**，日志procfs-diag-campaign.log：同ELF独立新boot，对旧brk-range用--observe记录负控制，对新内核严格断言。fixture stat/getpid、37字节分块读/seek稳定、950..1023明确未注册调用ENOSYS计数尾部、write-only不arm、lockfooter；使用preserve模式避免runner自动状态干预。结果目录perf-diag-probe-old/new。完成前不要重启同实验；之后检查输出再跑有guard的getpid/stress生命周期和guest回归/正式APK复测。

## 2026-10-22 21:43 北京时间：诊断候选最终复测正在进行

- 初探修正：950..1023并非全空，1000+存在兼容别名；初始两次fixture错误已保留，不算成功。最终仅调用源码核实为空的950..999，全部ENOSYS。perf的syscall IDs是canonical generic，x86 getpid39映射172，fixture也已修正。
- 另发现真实生产路径：sys_fstatat/statx对无执行位普通文件调用shebang检测，会open/read /proc/a20/perf，即使metadata长度0仍arm。新增安全短路st_size<2（不可能是shebang）；真实C helper ASan/UBSan测试保证两字节#!仍获原兼容执行位、短读/失败/已有执行位/非regular不变。
- 最终新kernel procfs-diag-v2.elf c4e77647183d4891c0bb5b533a368967a7edfcb273f8f59842b060be260e8825。独立fresh guest v4负控制旧版stat后getpid1000次被计数、快照4095截断；新版stat_size0、initial_state0/getpid0、4515字节快照含950..999全部计数、writeonly不arm、37字节分块/seek稳定、lockfooter完整。严格PASS。
- 新runner实际生命周期guard已验证：每测项前disable，后审计再disable；getpid三段后状态都0，stress-fork后也0。但getpid仍616.73→1002.34→827.62ns，说明剩余时段/工作负载后效不能全归因于perf开关，不能宣称已经消除所有抖动。
- 新内核六项guest回归MM/SCHED/FUTEX/GS/VFS_CONC/FSYNC全部0；FSYNC自身明确开启计数，审计标1，其余按真实状态记录。
- **当前exec session23476**，日志build/performance/procfs-diag-final-campaign.log：旧brk-range/新procfs-diag-v2 七项完整APK按AB/BA/AB，共84样本，5秒同参数，使用新runner明确关闭和测后审计。输出perf-apk-diag-paired-{1,2,3}-{old,new}。不要编译或并行测性能。配置/buildinput ELF symlink已冻结。等完成检查84样本+审计+哈希、归档诊断/全套对照并提交此候选。
- 下一个潜在方向是dense COW重复root walk；先结束当前候选、保存回退点再改。另核查core/perf固定32CPU数组：MCU trim实际不含该模块，按用户要求不为此引入无用MCU双实现。

## 2026-10-22 21:50 北京时间

- 诊断候选最终全套APK对照84/84完成；新版21个guest测项计数审计全0，全部输入hash未变。原始/失败初探/host/六项guest及全套APK归档procfs-diag-2026-10-22*，报告第10节完整。
- 当前新内核对Linux中位：CPU1/2t约1.006/1.007、memory-write0.867、共享锁0.297、fork0.151、SHA0.890。SysV1.068但时钟/调度语义及范围影响仍需保留，不能称全系统领先。前后CPU控制也有7%-9%逐对差异，宿主其它独立KVM/TCG实验仍存在，不把内存/密码学的宽范围吞吐差异归因于本补丁。
- 诊断候选准备提交；所有本任务性能/构建进程已结束。下一步沿当前c4e776...内核作为冻结baseline，检查dense COW share snapshot/downgrade/unlock重复PT root walks。若尝试，保持父锁/引用固定/锁token一致、失败回滚及同参数实测，不复活先前无一致收益的probe-only试验。

## 2026-10-22 21:59 北京时间：COW 节点 token 候选正在复测

- 诊断修复已提交e98aea06，frozen baseline为procfs-diag-v2.elf c4e776...，之前所有84APK及新状态审计原始证据已提交。
- COW候选：将unlocked Src probe与64B SharedSnapshot区分，snapshot携带确切slot/node，downgrade/status/unlock复用这些指针，避免3次root walk。初次probe/节点锁内重读+引用固定不改，不是复活之前被拒绝的probe-only改动。Rust SharedSnapshot不Copy，C/RS ABI断言64B(LP64)/40B(ILP32)，普通probe ABI保留。
- host Rust事务83checks通过，检查child map发生在unlock之后、失败只释放正确pin。新增真实C adapter ASan/UBSan测试通过：原slot/node身份、巨大页状态、匿名frame/cache pin/VMO无frame引用、leaf消失释放锁；fixture首次变量名puts冲突编译失败已保留，修正后PASS。checked range双swap各20万回归通过。只构建x86。
- kernel保存cow-token.elf adbcbbdf0e203c163790490a50dca77a769cb22391f06ca2352f4a2f63fee8f3，尚未提交候选。
- **当前exec session74336**，日志cow-token-campaign.log：先MM/process-vm-exec-COW/TLB teardown/swap四项guest，成功后baseline/cow-token五对AB/BA/AB/BA/AB，配置cow-token-config.json，9case（getpid首尾、最小fork、3种brk、mmap touched16/64、stress fork），共180性能样本。新runner明确关闭+测后审计，要求guest状态全0；期间不要编译或并行性能。输出perf-cow-token-regression与perf-cow-token-paired-*。
- 下一项可审查重复fork对already-COW叶项是否做无意义相同PTE写/状态刷新/TLB note；mm_cow_flags会清D，要先确保等价比较和状态一致性，不直接跳过COW标记页。先完成当前单项ablation。

## 2026-10-22 22:07 北京时间

- COW token完整180样本完成，90guest状态全0、base unchanged。64MiB总16.1586→15.8014ms(-2.21%)五对同向；父fork阶段9.3265→8.6593ms(-7.15%)五对同向，wait+6.62%抵消部分。stress fork605.21→644.21(+6.44%)五对同向。首末getpid控制-3.45%/+3.18%，保留对收益精度的限制，详见报告第11节。
- cow-token-2026-10-22*已归档，准备提交节点复用作为独立回退点。接下来单独试验downgrade仅在编码后PTE不同才写并note TLB，状态sync始终保留，不能直接按COW位跳过，因为mm_cow_flags会清D。该实验尚未写代码，先保留当前snapshot/锁顺序。

## 2026-10-22 22:24 北京时间：PTE实验已回退；fileio初测完成

- 相同PTE写/免TLB-note实验180样本全0且正确性过，但性能方向混合（mmap16中位+0.43%、64中位-2.18%却前三对微退化、brk空堆+8.62%）；已保存tools/bottleneck-cow-identical-pte.patch及cow-same-pte-2026-10-22*证据，生产cow.c/fixture恢复HEAD cd49ffff。默认build ELF尚是被拒实验，后续make需重建；显式frozen cow-token.elf adbc才是当前已保留实现。
- sysbench fileio初测已完成perf-fileio-initial，2case×Linux/A20均成功：1线程、64MiB、4KiB随机写、validate、fsync频率100/16384、5秒，prepare后busybox sync指定文件、测后16384次seqrd覆盖完整64MiB校验并清理；f_type ef53。A20约19.49/56.24MiB/s，Linux162.39/201.96，guest计数审计0。此时尚未隔离RB算法增益。
- 后续计划只临时替换page_cache_state.rs中dirty_insert/remove为a9b7d468旧链表函数，保留当前所有新C/RS descriptor字段/ABI和其它修复，构建linear-control；还原源码后重建index-current，核对还原ELF一致。这样隔离算法，避免拿旧内核隐式profiling/旧MM等混杂影响fileio比较。保留源备份/patch/sha，控制版不提交或发布。随后三对同参数fileio并全数据校验，raw out按perf-fileio-paired-*保存。当前所有本任务perf进程已结束，fileio初探session73076完成。

## 2026-10-22 22:28 北京时间：fileio算法隔离对照正在运行

- 回退后的默认内核已重新构建为已保留cow-token adbc...；控制只替换page_cache_state.rs的dirty_insert/remove为a9b7d468旧函数，所有descriptor/vnode字段和其它修复保留，因此不是memory版。脚本build/performance/build-fileio-control.py已在finally还原源码，前后ELF逐字节相同；manifest和两份冻结rs位于build/performance/fileio-control。
- linear ELF46bceb43e0e9a8acefe0ed0587eac9b0b1e826eaca1f29486bd8d5f55e07816d，index ELFadbc...。默认源无生产未提交修改，linear控制版不提交/发布。
- **当前exec session21032**，日志fileio-paired-campaign.log：先linear控制MM/swap回归，再两种freq100/16384的APK sysbench fileio AB/BA/AB三对24样本；out perf-fileio-linear-regression和perf-fileio-paired-*。16个APK/库+busybox hash已核对baseline，并要求前后不变；每次64MiB、4KiB buffered rndwr、1t、5s、seed271828，prepare后sync指定文件，run后完整64MiB seqrd+validate校验和目录清理。guest计数要求测后0。
- 当前期间不要编译或并行性能测量。等完成解析FILEIO_RUN_BEGIN/END之间的吞吐（不要误用后面verify阶段的seqrd速率），核对每段CRC完整校验与cleanup。准备归档控制源码/patch/哈希、24样本与报告第13节，并提交已预验证wrapper工具。
- 中间有机会用已有诊断接口独立测量freq100低吞吐的flush/writeback/设备轮询计数，诊断吞吐不与正式吞吐混算。截止UTC16:00前继续推进，届时不再开始新实验。

## 2026-10-22 22:43 北京时间：fileio完成并归档

- fileio三对24样本全部成功+CRC完整校验/清理/guest状态0，16个APK库及busybox前后hash一致。linear控制源码只换旧dirtyinsert/remove、ABI/layout与index一致，测试后默认ELF仍adbc，与最初还原前相同。
- freq100 linear20.34→index19.93MiB/s(-2.02%)；freq16384 44.71→55.51(+24.16%)但逐对-19.24/+27.60/+33.28%，不当成一致应用改善。Linux freq100控制也-28%，硬盘/宿主其它独立TCG任务影响明显。完整fileio-2026-10-22*和报告第13节已写。
- 独立diag完成perf-fileio-diagnostic：两个窗口分别260/8次fsync，经过时间4.721/4.777秒（包含等待、非CPU占比），约25.8k块写回IO，42k/68k页，usedchecks/completion约6，bouncebytes0。窗口包含prepare/verify，不能全部归给RUN；暂未证实cache扫描而非同步小IO主导。源码blockcachepool实际2000页，位图等变化需先有阶段证据，暂不贸然改。
- 当前本任务所有构建/性能进程结束。下一项可低风险缓存COW class：snapshot已有锁定table/level及VMA，而mm_fork_page_class还rootwalk且fallback再次VMA查找。需真实class/backing/huge测试后单项同参对照。UTC16:00截止，之后不开始新实验。

## 2026-10-22 22:49 北京时间：COW cached class 候选正在复测

- fileio工具/原始24样本/隔离控制/诊断已提交9dcecfe6，主线已验证内核仍cow-token adbc...。PTE-identical实验已拒绝并回退，默认构建曾还原到adbc且与还原前逐字节相同。
- 当前C候选未提交：mm_fork_page_class直接使用snapshot已有locked table/level与已查到VMA，省一次root walk及缺元数据时的第二次VMA查找；有效class优先保留，fallback顺序不改，arm32无sidecar分支保持anon。Rust接口不变。
- 真实C adapter测试现纳入实际class helper和pt.h宏，涵盖4KiB/huge、metadata有效class优先、invalid/PT_NODE→VMO/file-shared/file-private/anon/noVMA、每次恰一次VMA lookup，ASan/UBSan通过；Rust事务83checks通过。x86构建完成cow-class.elf b39f1b3938032422a73363ab9efc17e24393a2a8626f30d4dd40f7fdfa82050f。
- **当前exec session90994**，日志cow-class-campaign.log：先4项COW guest回归，成功后cow-token/cow-class五对AB/BA/AB/BA/AB，沿用cow-token-config.json 9case共180样本，测项前关闭、要求测后0；输出perf-cow-class-regression和perf-cow-class-paired-*。期间不要编译或并行性能测试。
- 截止UTC16:00（北京时间00:00）仍约1小时，当前只跑这一项；完成后按数据决定保留，归档原始数据/patch/rollback，别因剩余时间急于提交不确定改变。

## 2026-10-22 23:02 北京时间：COW class 完成

- session90994已结束，无本任务构建/测试残留。180性能样本全部成功，90guest状态0，4guest回归和Rust83/真实C ASanUBSan通过，包含非零VA不同level索引。
- 保留已有table/level/VMA复用：64MiB总中位-1.97%四对下降，空brk-3.44%五对下降，stressfork+1.52%四对上升；16MiB mmap持平，稀疏brk+2.37%四对上升，明确不作普遍改善保证。报告第14节和cow-class-2026-10-22*已归档。
- 接下来补离线perf差分工具，再最终完整APK回归。截止UTC16:00仍有效，不推送/合并/部署。

## 2026-10-22 23:04 北京时间：离线分析工具

- 新增a20-perf-diff.py，支持完整文本对照和diagnostic manifest，canonical syscall映射、全部精确整数delta、elapsed ticks/每调用平均，不输出CPU百分比。拒绝明显截断/配对缺失/重置/停用状态；完整缺失尾部不可检测的限制已写明。
- 三项host验收全部通过，真实fileio两窗口260/8次fsync和原ticks精确一致。初始预期手工抄录错误已修正，非修改实测数据。
- 最终x86内核构建结束；接下来冻结cow-class ELF运行完整七项APK，期间不编译。

## 2026-10-22 23:06 北京时间：最终APK正在运行

- 当前exec session99202，日志build/performance/apk-final-campaign.log，冻结cow-class b39f1b...，七项APK、同核4/6、2vCPU2GiB、5秒、三轮，目录perf-apk-final。第一轮全部有效；期间不编译/另起性能实验。
- 已准备build/performance/build-final-profile.py（尚未执行）：重用旧coarse MM/fork/wait观测点，保留新版grow procfs，另外分解block-sync transaction/device-write/device-flush，带empty span粗略估计计时开销。只诊断、finally还原源码，再重建验证正式ELF逐字节一致。须等最终APK结束后才编译，诊断吞吐不能混入正式吞吐。

## 2026-10-22 23:10 北京时间：最终APK完成、分阶段诊断开始

- session99202完成，42/42有效、21guest状态0、输入未变；apk-final-2026-10-22*已归档。CPU约1.008倍Linux，线程0.293、fork0.141；fork315–671ops/s波动保留，不挑轮次。
- 临时诊断构建成功，ELF338cb9e331c1963c10371339e67ad8daf9c91001ff6455b721805ef459128872。finally源文件哈希全部还原且无g_bp符号残留。
- 预期“还原ELF逐字节相同”的断言失败：这次CFLAGS全量重编使version.h的__DATE__/__TIME__由20:53变23:08，字符串合并/rodata位置也改变。生产ELF349bb30409c53743efe1b312f4dade03529905d8c036555ef587c7994ca1bc83；全部函数符号地址/大小与冻结b39f1b完全相同，正式性能仍明确对应b39f1b。保留失败及重建日志，不把两种ELF冒称字节相同。final-rebuilt.elf另存待正确性检查。
- 当前exec session30573，日志final-profile-campaign.log，三个boot×6case，包含MM正确性、最小fork/稀疏brk/dense mmap和两种fileio。host /bin/true为占位，仅分析guest诊断，不作为性能比较；新增fileio-profile.sh在RUN前后各抓完整快照，避免把prepare/verify混入RUN阶段归因。源码已还原、无并行编译。

## 2026-10-22 23:12 北京时间：用户追加合并授权

- 用户明确要求：到午夜完成后合并性能worktree到main，然后删除worktree和branch。这覆盖之前“不得自行合并”的限制；仍不推送/部署。须先保存证据并正确处理main的现有用户改动。
- 当前main位于原checkout且有大量未提交修改/未跟踪基线文件，若干与性能分支重叠（Makefile/pagecache/syscall等）。不能覆盖或顺手提交这些无关工作。截止前先完成可审阅成果，截止后采用可恢复方式整合，保留原改动。
