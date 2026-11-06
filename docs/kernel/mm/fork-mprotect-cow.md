# Fork 与 mprotect 的 COW 并发约束

共享文件映射还必须保留 dirty-state 跨权限变化和 shutdown 的语义：已写入的共享页在`mprotect(PROT_READ)`、unmap 或关闭映射时不能因 dirty bit 丢失而漏写。RV64 曾出现“共享写入 → 只读保护 → clean shutdown → coldboot 读回全零”的数据丢失；已修复路径通过字节精确冷启动读回与 `e2fsck` 验证。该结论现在是可复跑门禁而不是一次性记录：`make smoke-mm-shared-write-coldboot` 对同一张 ext4 镜像启动两次，第一次覆盖“写后 `mprotect(PROT_READ)`”“写后 `munmap`”“映射在关机时仍然活着”三种形态并 `poweroff`，宿主机对镜像跑 `e2fsck -fn`，第二次冷启动逐字节读回比对；写入路径不调用 `msync`，要证明的正是 shutdown 路径自己保留了 dirty 状态。riscv64 与 aarch64 都通过。Linux RESTART 的 checked `vfs_sync` 不是完整 CLEAN shutdown；失败恢复后重新 fault 的 guest 覆盖仍是证据缺口。实现和验收边界见 [`fs-consistency-model.md`](../../fs/fs-consistency-model.md)。

fork 复制私有映射时，父页表的叶节点锁保护源 PTE 快照、子映射所需的帧引用和父 PTE 的 COW 降权。子页表映射在释放父叶节点锁后完成，避免在节点锁内分配页表；若子映射失败，父映射保留 COW 仍可安全写入。

`mprotect` 在写入权限前重新核对它先前观察到的 PTE。若并发 COW 已替换该项，它会重试同一虚拟地址。对 fork 后仍有多个引用的私有匿名页恢复写权限时，PTE 保留 COW，第一次写入仍会复制页面。

状态驱动的匿名 COW 路径在叶节点锁内核对 PTE 与状态字节，并在释放叶锁前持有旧帧引用；复制结束后通过比较并替换安装新帧。该路径与处理文件页及独占帧的回退路径共用地址空间锁，保证两个实现不会同时改写同一映射。

`smoke-mm-fork-exec-race` 除并发 fork 与 VMA 操作外，还运行确定性隔离检查：子进程对 fork 共享页依次执行只读和读写 `mprotect`，再写入该页；父进程在每轮后确认原值未改变。该检查覆盖恢复写权限不能清除仍有效的 COW 义务。

`pfn_valid()` 只表示 PFN 落在已登记的物理内存范围内，不检查帧当前是否分配或其引用数。帧的存活由 `pfa.meta[].refcount` 及持有的映射/临时引用保证；因此，PFN 范围检查本身不构成并发帧回收保护。

## RV64 内核 trap 的寄存器保存

RV64 内核态 trap 入口在分配 trap frame 后，需要先保存原始 `t0`，再借用它重建陷入前的 `sp`。通用寄存器保存循环跳过已单独保存的 `x5/t0`。此前循环把临时计算出的旧 `sp` 写进了 `t0` 槽，导致 timer IRQ 返回后被打断的内核代码拿到错误的 `t0`；context switch 路径中 `t0` 可暂存地址空间状态，错误恢复会扰乱后续控制流或页表切换。

`make smoke-rv64-trap-t0` 通过 `a20.trap_t0_selftest=1` 在 RV64 单核 guest 中运行哨兵测试。测试在 `t0` 持有固定值时等待本 CPU 的 supervisor timer IRQ，并要求中断处理程序确认 IRQ 确已发生且汇编探针返回时哨兵未变；超时、未收到本 CPU 的 IRQ 或寄存器值变化都会失败。MM 修复后的回归还应运行 `make -j8 smoke-mm-pt-race`、`make -j8 smoke-mm-fork-exec-race` 和 `make -j8 smoke-mm-stress`。

## fork 写保护与 TLB 完成边界

页表叶锁只保护 PTE，不能阻止另一 CPU 继续使用旧的可写 TLB。fork 释放地址空间锁后、远端 shootdown 完成前，COW 若提前复制旧帧，父进程其他线程随后写入旧帧的内容就会丢失。COW 现在先取得该地址空间的 `tlb_lock`，等待先前事务完成，再取得 `mm->lock`；复制、shootdown 和旧帧释放都在这一边界内。

缺页处理的第一次 COW 检查与最后的 present 检查之间也可能发生 fork。最后检查若见到 VMA 允许写入的 COW 叶，会重新执行 COW，避免把有效写入报告为 SIGSEGV。批量 TLB 完成路径则固定当前 CPU，直到本地刷新与各 CPU generation 记账完成；释放延迟帧前恢复抢占。

`vm_fault_test` 新增三个线程持续原子递增、主线程执行 64 次 fork 的检查。旧内核记录 `actual=26810828 expected=26812229`；修复后的完整 x86_64 smoke 记录 `actual=13837809 expected=13837809`，其余 MM、信号、GS、eventfd、SCM 和 socket 检查通过，关机审计为 0。宿主确定性交错检查运行命令为：

```sh
python3 tools/tests/test_tlb_transaction_races.py
```

本机原始日志保存在 `build/browser-followup/cow-race-negative-guest.log` 与`cow2-full-guest.log`，不纳入源码提交。

活 PTE 的拆除在 guest 侧由 `make smoke-mm-tlb-teardown` 把守：8 核 guest 上 3 个读者线程反复读主线程正在 `munmap` 的同一批页，读者只在“已填充”窗口内被放行；`munmap` 之后主线程立刻把刚释放的帧回收进另一个保留区间并写满毒值，因此“帧在 shootdown 完成前被释放并复用”会让还在使用旧 TLB 项的读者读到毒值而不是该页的填充值。该门禁还要求读者确实发生过缺页竞争、`MAP_FIXED_NOREPLACE`探针证明释放后不再映射、RSS 不增长，所以绿灯不可能来自线程从未相撞。负控已实测：去掉`mm_tlb_invalidate_finish()` 的远端 shootdown 后，读者观测到 573 次毒值，门禁转红。

## Reclaim hints retain live contents

Linux `MADV_COLD` and `MADV_PAGEOUT` previously entered the same PTE-discard loop as `MADV_DONTNEED`. Private anonymous data became zero on the next fault, and shared anonymous mappings could lose their backing as well. These hints now validate the range and reject locked/PFN mappings, while keeping resident contents intact. This is a conservative hint implementation, consistent with the Native ABI; it does not force per-range swapout or implement page aging. The existing global pressure reclaimer remains responsible for eviction. See [madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html).

`vm_fault_test` fills both private and shared anonymous ranges, issues both hints, checks every word, and writes a new pattern between hints. With the previous kernel the added test reports `reclaim-hints FAIL` (`build/browser-followup/reclaim-negative-guest.log`). The corrected x86_64 and RISC-V 4-CPU preemptible kernels and a Linux host pass; the full x86 smoke also passes its signal, COW, eventfd, Unix descriptor passing, socket registry and network option probes (`reclaim-full-guest.log`, `reclaim-host.log`, `reclaim-rv-guest.log`). This establishes the data-loss regression, not a diagnosis of Firefox's remaining heap crash.

## User copies and sleeping positional I/O

User-copy helpers now hold the address-space lock from translation lookup through one base-page copy (also for huge mappings). This prevents COW, unmap or remap from retiring the translated frame before the copy finishes. Fault resolution runs outside the lock, followed by a fresh permission and translation lookup. The same lifetime rule covers user string helpers and records accessed/dirty bits when the direct map is used.

The internal `user_buffer_segment` borrowed-pointer interface has been removed. Its sole caller, positional AIO, now uses a kernel bounce page: device I/O may sleep while another thread changes the submitting process's mappings. User copies run outside the file-position lock, allowing faults on mappings of that same file, and each chunk saves/restores the shared file position under its lock.

Deterministic host probes exercise a pending mapping replacement at the copy boundary and a replacement during VFS I/O:

```sh
python3 tools/tests/test_usercopy_mapping_races.py
python3 tools/tests/test_aio_user_mapping_races.py
```

The 4-CPU preemptible x86_64 full VM smoke and RISC-V VM/AIO probes pass (`build/browser-followup/usercopy-aio-guest.log`, `usercopy-rv-guest.log`). The added guest `copy-protection` case also passed on the preceding kernel; it is permission regression coverage, not evidence of reproducing the copy race. Firefox ESR still crashes after this change, so this fix does not establish the cause of its heap corruption.

ARM32 cross-compilation additionally caught a direct conversion from the 64-bit user-address arithmetic to a native pointer. Converting through `uintptr_t` keeps that generic helper valid for 32-bit kernels. The ARM32 Linux kernel build passes with this conversion; the AArch64 dual-ABI kernel also builds. These are compile checks, not desktop or boot validation for either architecture.

## 可恢复页与共享匿名后备

普通 MMU 上的 Linux `MAP_SHARED | MAP_ANONYMOUS` 使用内部 `VMO_SHARED_ANON` 后备。创建映射只预留地址范围；Rust 稀疏页索引在首次触页时预留元数据，随后完成 cgroup 计费、帧分配、清零与唯一发布。VMA 持有后备引用，PTE 只借用后备帧。分裂、别名与重映射必须保留 `backing_offset`；`mremap` 扩展可增加虚拟范围，超出原后备范围的访问仍产生 SIGBUS。NOMMU、原生 capability 映射及显式 huge-page 映射保留各自路径。

每个首次分配页面的 cgroup 在后备中保有引用及页数记录，最后一个后备引用销毁时才退还预算。进程退出按 RSS 进行的旧计费回退不得扣除这部分对象所有的预算。最终 MM 拆除先完成 TLB 同步，再遍历页表释放普通帧；`MM_ST_VMO` 叶不减帧引用，后备的最终释放统一回收 canonical 页面。`mincore` 查询后备驻留状态，因此缺少本映射的 PTE 不等于页面未驻留。

fork 可以省略可从后备恢复的子 PTE。目前仅对至少 1 MiB、非可执行且架构允许重新缺页的映射使用该策略。共享匿名 VMO 可以直接跳过现有叶；私有文件叶还必须只读、属于文件私有类、没有特殊状态，且 PFN 与对应 page-cache owner 相同。已经私有修改、随后改回只读的文件页仍须继承。小映射和可执行映射保留现有叶，避免放大常用代码及栈的缺页成本。

共享匿名 VMO 缺页后，还会尝试映射后续至多 31 个连续驻留页面；遇到后备空洞、VMA/后备边界或页表分配失败即停止，已有 PTE 保留。该窗口不分配 canonical 页面、不新增 cgroup 计费或帧引用，借用帧仍由 VMA 持有的后备引用保护。

驻留页窗口通过同一个地址空间游标，按叶页表节点批量安装 PTE、更新状态和记录 cache owner。已有非零 PTE（包括 swap 项）不覆盖；中途分配失败保留已完成部分，只有实际安装的输入槽会被清空，调用者据此统计 RSS、转移引用和清理未消费的页面。私有文件页保持只读 COW，并转移一份 page-cache pin；共享匿名页只借用 VMO 帧，不能增加或释放帧引用。VMO 查询先于页表加锁，避免引入页表锁到 VMO 锁的顺序依赖。可执行 VMO 页仍须在发布 PTE 前同步指令缓存。

文件映射的冷页填充窗口保持 16 页。当同一 VMA 内的前一页已经映射，且映射为非可执行、只读私有映射时，窗口可扩展到 64 页；第 17 页起只查询已驻留且 uptodate 的缓存页，遇到缺页即停止，不扩大磁盘读取或缓存分配窗口。孤立访问仍用 16 页，避免用顺序读取收益换取稀疏读取退化。窗口边界在 I/O 后依据当前 VMA 再验证。sealed VMA 不使用推测映射，批量安装也尊重叶节点安全位。

文件 fork 的叶节点检查可以先作只读范围证明：在同一个源节点锁内，验证当前 VMA 范围中的所有非零叶都只读、具有文件私有状态、没有安全标记，且 cache owner 的 PFN 与 PTE 一致。全部满足时直接略过该范围，不再创建空的 Rust 克隆事务或检查子页表。任一页面不满足则回退到常规克隆，保留修改后又改为只读的私有页。完整 L0 页表通过验证后缓存正向结果，后续 fork 仍检查当前 VMA 策略，但无需再次逐页检查。首次验证与失效后重建仍为线性扫描；未覆盖整张表的边界范围不产生摘要，可以使用已有的完整摘要。稳定映射的重复 fork 因此按叶页表数量检查，而非按驻留页数检查。

摘要只证明页表自身的 PTE、类别、安全位和 cache owner，不缓存 VMA 的文件身份或策略。普通节点写锁在取得锁后撤销摘要；状态、权限、安全位、元数据复制和失效节点更新也显式撤销，覆盖持有 mm->lock 的旧调用路径。唯一保留摘要的观察锁仍是互斥 MCS 锁，并保持原有禁止抢占与 CPU 固定规则。fork 的观察同时持有 parent->lock；任何元数据或 PTE 写入都必须由普通节点锁或已有地址空间锁协议保护，不能绕过这些失效路径。

遍历每层页表时直接计算与范围重叠的首槽位，保留宽整数边界检查和部分大页的原有规则。空 PTE 在进入批量事务前跳过，避免对稀疏叶表的空前缀创建 Rust 守卫、探测子页表和取得源节点锁；非零交换项继续走原有路径。

普通克隆事务最多处理 64 页；C 与 Rust 的容量必须一致，且批量不能跨叶页表或 VMA 边界。失败时未转移的帧引用和缓存页 pin 由事务守卫回收。父 PTE 已等于目标 COW 编码时只同步软件状态，不重复写 PTE 或记录 TLB 失效；实际权限变更仍遵守原有 shootdown 时序。

私有文件页发生实际 COW 复制后，新页面是进程所有的匿名帧。状态更新必须清除旧 cache owner，并记录 `MM_ST_ANON_MAPPED`；旧缓存页引用仍由调用者在 TLB 同步后释放。后续 fork、COW 和最终拆除据此使用帧所有权路径，不再因 VMA 仍属文件映射而反复查询 page cache。仅做权限更新或共享文件写入不能据此改变所有权类别。

这项策略将页表复制成本移到子进程后续访问。fork 后退出或 exec 的负载通常受益；子进程立即读取全部映射会产生更多缺页，可能更慢。评估时必须同时测量两种访问模式，不能仅凭 fork 返回时间判断整体收益。

宿主检查覆盖真实 C/Rust 适配器的分配失败、并发首次触页、计费与生命周期；guest 检查覆盖内容、权限、别名、驻留、COW、帧复用和回收事件。可执行：

```sh
python3 tools/tests/test_file_cache_window.py
python3 tools/tests/test_shared_fault_around.py
python3 tools/tests/test_shared_anon_host.py
python3 tools/tests/test_vmo_page_host.py
python3 tools/tests/test_anon_final_destroy_host.py
python3 tools/tests/test_cow_snapshot_token.py
python3 tools/tests/test_pt_mcs_preempt_window.py
python3 tools/tests/test_pt_cache_owner.py
python3 tools/tests/test_cow_clone_range.py
python3 tools/tests/test_cow_private_copy_status.py
python3 tools/tests/test_cow_anon_child_batch.py
python3 tools/tests/test_reap_ready_host.py
bash tools/host-tests/test-mmap-policy-host.sh
bash tools/host-tests/test-mseal-policy-host.sh
# 已构建本仓库 x86_64 musl 后，生成两侧 guest 共用的静态程序：
bash tools/host-tests/build-fork-exit-probes.sh
```

`fork-exit-bench` 接受 `file|shared-sparse|shared-dense`、轮数、MiB、子进程读取模式四个参数（0：不读；1：逐页读取；2：每隔 1 MiB 读取；3：只读首个页面）。父进程在计时前触页（`shared-sparse` 除外）；结果包含整个 fork/wait 周期和父进程侧累计 fork、wait 时间。两侧应使用同一二进制、相同 vCPU/内存/CPU affinity 和透明大页设置，并把诊断计数与正式计时分开。

回收完成还有独立于页表的 CPU 生命周期边界：`ZOMBIE` 不意味着退出任务已经离开 CPU。阻塞 wait 在子任务的 `park_lock` 内注册完成通知，CPU slot 的任务引用移交给完成回调，保护并发回收。SIGCHLD 与 pidfd 可读状态必须等到对应进程可以非阻塞回收后才可见；线程组信号若先由最后一个 sibling 产生，还须等待仍在 CPU 上的 leader。原生任务对象的逐任务 EXITED 事件也移到该任务释放 CPU 之后，保持原有的任务与线程组事件范围。

## 首次文件 fork、借用帧批处理与扩大性能矩阵

只在第一次 fork 扫描后缓存证明，仍会让第一次 fork 与文件驻留页数线性相关。
`map_resident_window()` 现在在安装 canonical、只读、非执行的私有文件缓存页时增量建立
`fork_file_clean`：普通节点锁先撤销证明并记录本次持锁前的值；专用安装器仅在此前已有
证明或整个叶表没有映射/状态条目时恢复它。该安装器只填空槽，不覆盖已有内容；普通写入、
私有 COW、保护和安全位变更继续撤销证明。锁前快照仅供当前持锁者使用，不能跨解锁读取。
这让已知干净的整张叶表在首次 fork 即可跳过扫描；混合、脏页和不满足策略的范围仍走验证路径。

状态缺页入口过去会先获取可写 cursor，再发现文件/VMO 缺页不适用，从而在每次触页时
无意义地撤销证明。`mm_pt_anon_status_candidate()` 改用页表读侧生命周期保护和节点观察锁
执行否定预检，不分配页表。肯定结果只是提示，原 cursor 路径仍重新验证状态、权限和安全位。
不能将预检结果作为绕过 VMA 或 UFFD 检查的授权。

小型共享 VMO 的叶现在也可以批量克隆。子 VMA 已持有 VMO，`MM_ST_VMO` PTE 借用帧，
不获得普通 PTE 所有的 frame/cache pin；Rust 失败回滚也不释放这些借用帧。私有 VMO 模式、
混合状态和安全位不满足条件时回退。子映射保留共享可写权限。旧式 brk/stack 克隆只遍历
VMA 未覆盖的区间；无 VMA 批处理还须证明整个批次不穿过任何 VMA，并只接受匿名映射状态。

扩大测试还发现高 FD 数退出反复扫描空文件锁表。文件锁表现在维护原子发布的条目数，
无 POSIX 锁时进程/每 FD 清理直接返回；非空表仍持原锁执行原有操作。最终文件关闭仍处理
OFD 和 BSD flock，不能只因 POSIX 表为空就跳过。随机分裂/合并、容量/OOM 回滚、并发清理
和 guest 跨 fork 生命周期测试验证计数与所有权。

`tools/tests/fork_matrix.c` 与 `tools/host-tests/bench-fork-matrix.py` 提供 58 项共同 Linux/A20
负载，覆盖 1/16/64 MiB、私有文件/匿名/共享/稀疏映射、子进程无访问/读/写、首次 fork、
256/1024 个 VMA、64/512/4090 个 FD、2/4 个 worker、8/32/128 个突发子进程、wait-any/wait-pid、
exec 和 stress-ng。fork/wait 延迟包含子进程工作，不等于单独 fork 系统调用耗时；并发另报
墙钟吞吐。`file-first` 每次重新 mmap 和预触页，单独记录 setup（含 unmap）与总耗时，
避免把转移到准备/拆除阶段的成本隐藏掉。4090 个 FD 给标准描述符留出空间，4096 会触及
当前固定 `MAX_FILES=4096` 限制。

复现时先运行 `tools/host-tests/build-fork-exit-probes.sh OUT_DIR`，然后使用：

```sh
python3 tools/host-tests/bench-fork-matrix.py \
  --image BENCHMARK_EXT4 --fat BOOT_FAT \
  --linux-kernel VMLINUZ --linux-initrd INITRD \
  --probe OUT_DIR/fork-matrix --kernel ABSOLUTE_KERNEL_ELF \
  --out NEW_RESULT_DIR --cpus 4,6 --smp 2 --runs 3
```

脚本使用镜像副本和 QEMU snapshot，记录输入散列并核验前后不变；相同静态探针在两个系统
执行，交替启动顺序，Linux 关闭 THP，正式测量关闭并审计 A20 计数器。`--diag` 单独归因，
不能与正式计时混合。四核应同时增加可用宿主 CPU，例如 `--cpus 4,6,8,10 --smp 4`。
新增证明命中/扫描数、批量/单页克隆时间和子页表安装时间；安装时间嵌套于批量和克隆时间，
不能重复相加。

2026-11-06 的两核三轮矩阵中，首次 64 MiB 文件 fork/wait 从 365 降到 175 µs，
4090 FD 从 1391 降到 519 µs；首次文件证明扫描计数为零。三组交替旧/新内核的 stress
提升 6.2%、8.5%、6.8%。完整矩阵 stress 中位吞吐为 4098 ops/s，Linux 5499 ops/s；
首次文件包含 mmap/prefault/unmap 的完整迭代仍为 19.099 ms，Linux 5.721 ms。
因此尚未达到全面 Linux 水平。58 项没有超过 10% 的中位耗时退化，但三轮和未锁频环境
不足以证明所有工作负载都无退化。完整样本、范围、吞吐、诊断和剩余瓶颈保存在
`build/fork-fast/wide-results.json`、`wide-report.txt` 与各原始 manifest/串口日志中。

四核另做一轮 58 项双系统矩阵（116 个样本）及六组 guest 回归，均通过；它与两核三轮
中位数分开报告。四 worker 文件 fork 吞吐为 10637 ops/s，Linux 19280 ops/s，仍有
明显并发差距。23 项宿主回归和 x86 SMP4/AArch64/RISC-V 双 ABI 构建通过；后两者仅编译验证。
