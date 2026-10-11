# Fork 与 mprotect 的 COW 并发约束

fork 复制私有映射时，父页表的叶节点锁保护源 PTE 快照、子映射所需的帧引用和父 PTE 的 COW 降权。子页表映射在释放父叶节点锁后完成，避免在节点锁内分配页表；若子映射失败，父映射保留 COW 仍可安全写入。

`mprotect` 在写入权限前重新核对它先前观察到的 PTE。若并发 COW 已替换该项，它会重试同一虚拟地址。对 fork 后仍有多个引用的私有匿名页恢复写权限时，PTE 保留 COW，第一次写入仍会复制页面。

状态驱动的匿名 COW 路径在叶节点锁内核对 PTE 与状态字节，并在释放叶锁前持有旧帧引用；复制结束后通过比较并替换安装新帧。该路径与处理文件页及独占帧的回退路径共用地址空间锁，保证两个实现不会同时改写同一映射。

`smoke-mm-fork-exec-race` 除并发 fork 与 VMA 操作外，还运行确定性隔离检查：子进程对 fork 共享页依次执行只读和读写 `mprotect`，再写入该页；父进程在每轮后确认原值未改变。该检查覆盖恢复写权限不能清除仍有效的 COW 义务。

`pfn_valid()` 只表示 PFN 落在已登记的物理内存范围内，不检查帧当前是否分配或其引用数。帧的存活由 `pfa.meta[].refcount` 及持有的映射/临时引用保证；因此，PFN 范围检查本身不构成并发帧回收保护。

## RV64 内核 trap 的寄存器保存

RV64 内核态 trap 入口在分配 trap frame 后，需要先保存原始 `t0`，再借用它重建陷入前的 `sp`。通用寄存器保存循环跳过已单独保存的 `x5/t0`。此前循环把临时计算出的旧 `sp` 写进了 `t0` 槽，导致 timer IRQ 返回后被打断的内核代码拿到错误的 `t0`；context switch 路径中 `t0` 可暂存地址空间状态，错误恢复会扰乱后续控制流或页表切换。

`make smoke-rv64-trap-t0` 通过 `a20.trap_t0_selftest=1` 在 RV64 单核 guest 中运行哨兵测试。测试在 `t0` 持有固定值时等待本 CPU 的 supervisor timer IRQ，并要求中断处理程序确认 IRQ 确已发生且汇编探针返回时哨兵未变；超时、未收到本 CPU 的 IRQ 或寄存器值变化都会失败。MM 修复后的回归还应运行 `make -j8 smoke-mm-pt-race`、`make -j8 smoke-mm-fork-exec-race` 和 `make -j8 smoke-mm-stress`。

## fork 写保护与 TLB 完成边界

页表叶锁只保护 PTE，不能阻止另一 CPU 继续使用旧的可写 TLB。fork 释放
地址空间锁后、远端 shootdown 完成前，COW 若提前复制旧帧，父进程其他线程
随后写入旧帧的内容就会丢失。COW 现在先取得该地址空间的 `tlb_lock`，等待
先前事务完成，再取得 `mm->lock`；复制、shootdown 和旧帧释放都在这一边界内。

缺页处理的第一次 COW 检查与最后的 present 检查之间也可能发生 fork。最后
检查若见到 VMA 允许写入的 COW 叶，会重新执行 COW，避免把有效写入报告为
SIGSEGV。批量 TLB 完成路径则固定当前 CPU，直到本地刷新与各 CPU generation
记账完成；释放延迟帧前恢复抢占。

`vm_fault_test` 新增三个线程持续原子递增、主线程执行 64 次 fork 的检查。
旧内核记录 `actual=26810828 expected=26812229`；修复后的完整 x86_64 smoke
记录 `actual=13837809 expected=13837809`，其余 MM、信号、GS、eventfd、SCM
和 socket 检查通过，关机审计为 0。宿主确定性交错检查运行命令为：

```sh
python3 tools/tests/test_tlb_transaction_races.py
```

本机原始日志保存在 `build/browser-followup/cow-race-negative-guest.log` 与
`cow2-full-guest.log`，不纳入源码提交。

## Reclaim hints retain live contents

Linux `MADV_COLD` and `MADV_PAGEOUT` previously entered the same PTE-discard
loop as `MADV_DONTNEED`. Private anonymous data became zero on the next fault,
and shared anonymous mappings could lose their backing as well. These hints
now validate the range and reject locked/PFN mappings, while keeping resident
contents intact. This is a conservative hint implementation, consistent with
the Native ABI; it does not force per-range swapout or implement page aging.
The existing global pressure reclaimer remains responsible for eviction.
See [madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html).

`vm_fault_test` fills both private and shared anonymous ranges, issues both
hints, checks every word, and writes a new pattern between hints. With the
previous kernel the added test reports `reclaim-hints FAIL`
(`build/browser-followup/reclaim-negative-guest.log`). The corrected x86_64
and RISC-V 4-CPU preemptible kernels and a Linux host pass; the full x86 smoke also passes
its signal, COW, eventfd, Unix descriptor passing, socket registry and network
option probes (`reclaim-full-guest.log`, `reclaim-host.log`, `reclaim-rv-guest.log`). This establishes
the data-loss regression, not a diagnosis of Firefox's remaining heap crash.

## User copies and sleeping positional I/O

User-copy helpers now hold the address-space lock from translation lookup through
one base-page copy (also for huge mappings). This prevents COW, unmap or remap
from retiring the translated frame before the copy finishes. Fault resolution
runs outside the lock, followed by a fresh permission and translation lookup.
The same lifetime rule covers user string helpers and records accessed/dirty
bits when the direct map is used.

The internal `user_buffer_segment` borrowed-pointer interface has been removed.
Its sole caller, positional AIO, now uses a kernel bounce page: device I/O may
sleep while another thread changes the submitting process's mappings. User
copies run outside the file-position lock, allowing faults on mappings of that
same file, and each chunk saves/restores the shared file position under its lock.

Deterministic host probes exercise a pending mapping replacement at the copy
boundary and a replacement during VFS I/O:

```sh
python3 tools/tests/test_usercopy_mapping_races.py
python3 tools/tests/test_aio_user_mapping_races.py
```

The 4-CPU preemptible x86_64 full VM smoke and RISC-V VM/AIO probes pass
(`build/browser-followup/usercopy-aio-guest.log`, `usercopy-rv-guest.log`). The
added guest `copy-protection` case also passed on the preceding kernel; it is
permission regression coverage, not evidence of reproducing the copy race.
Firefox ESR still crashes after this change, so this fix does not establish the
cause of its heap corruption.
