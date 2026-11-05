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

这项策略将页表复制成本移到子进程后续访问。fork 后退出或 exec 的负载通常受益；子进程立即读取全部映射会产生更多缺页，可能更慢。评估时必须同时测量两种访问模式，不能仅凭 fork 返回时间判断整体收益。

宿主检查覆盖真实 C/Rust 适配器的分配失败、并发首次触页、计费与生命周期；guest 检查覆盖内容、权限、别名、驻留、COW、帧复用和回收事件。可执行：

```sh
python3 tools/tests/test_shared_anon_host.py
python3 tools/tests/test_vmo_page_host.py
python3 tools/tests/test_anon_final_destroy_host.py
python3 tools/tests/test_cow_snapshot_token.py
python3 tools/tests/test_reap_ready_host.py
bash tools/host-tests/test-mmap-policy-host.sh
bash tools/host-tests/test-mseal-policy-host.sh
# 已构建本仓库 x86_64 musl 后，生成两侧 guest 共用的静态程序：
bash tools/host-tests/build-fork-exit-probes.sh
```

`fork-exit-bench` 接受 `file|shared-sparse|shared-dense`、轮数、MiB、子进程是否读取每页四个参数。父进程在计时前触页（`shared-sparse` 除外）；结果包含整个 fork/wait 周期和父进程侧累计 fork、wait 时间。两侧应使用同一二进制、相同 vCPU/内存/CPU affinity 和透明大页设置，并把诊断计数与正式计时分开。

回收完成还有独立于页表的 CPU 生命周期边界：`ZOMBIE` 不意味着退出任务已经离开 CPU。阻塞 wait 在子任务的 `park_lock` 内注册完成通知，CPU slot 的任务引用移交给完成回调，保护并发回收。SIGCHLD 与 pidfd 可读状态必须等到对应进程可以非阻塞回收后才可见；线程组信号若先由最后一个 sibling 产生，还须等待仍在 CPU 上的 leader。原生任务对象的逐任务 EXITED 事件也移到该任务释放 CPU 之后，保持原有的任务与线程组事件范围。
