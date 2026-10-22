# MM 虚拟内存事务、fault 与回收的验证范围

> 仍未完成的事项见 [1.0 缺口台账](../gaps/mm.md)；本页只描述当前状态。

状态：**开放**。私有匿名、换出源、共享文件与注入回滚四类 DONTUNMAP 切片，活 PTE 的 TLB
拆除，分配失败三阶段回滚，以及共享写的冷启动读回都有可复跑门禁。以下结论来自 QEMU 功能运行，
不是真机结果、性能对照，也不是已认证的 C0 基线。

## 门禁与它们各自断言什么

| 门禁 | 覆盖 | 关键断言 |
| --- | --- | --- |
| `smoke-mm-dontunmap` | 私有匿名 DONTUNMAP | 目标数据保留、源 refault 按零页、fork/COW 语义、RSS 清理、`[MM-ASM]` 审计全零 |
| `smoke-swap` | 换出源上的 DONTUNMAP | 见下节 |
| `smoke-mm-dontunmap-shared-rollback` | 共享文件的注入失败路径 | 源字节与可写性保留、目标被清除、页缓存 pin 回到基线、同进程重试成功 |
| `smoke-mm-dontunmap-race` | DONTUNMAP 与并发读者 | 搬移字节完整、被清空的源按零页 refault、读者观测到的每个字节都来自填充、读者通过次数非零（非空） |
| `smoke-mm-tlb-teardown` | 活 PTE 的 TLB 拆除 | 见下节 |
| `smoke-mm-dontunmap-rollback-leaf1/2`、`-split-fail-1..6` | 目标 leaf 与 VMA split 的注入失败 | 源保留、目标清除、可重试 |
| `smoke-mm-alloc-fail` | 分配失败的三个独立阶段 | 见下节 |
| `smoke-mm-shared-write-coldboot` | 共享写的冷启动读回 | 见下节 |

## `MREMAP_DONTUNMAP` 的语义契约

`MREMAP_DONTUNMAP` 把源映射的页搬到新地址，但保留源 VMA 与其中计数。验证覆盖：

- 目标数据保留：搬移后目标映射读到与源一致的内容。
- 源 refault：源地址在搬移后按零页重新缺页，且不复活旧数据。
- fork/COW：搬移之后 fork 与 COW 的语义与普通匿名页一致。
- RSS 清理：多次搬移之后 RSS 回到与操作计数相符的水平，而不是单调增长。
- 确定性回滚：目标 leaf 1、leaf 2 之后注入一次性失败时，源数据与可写性、RSS 保持不变，
  目标被清除，同一进程可以重试成功。
- split 失败：Rust 目标 split 失败 1/2 走显式部分回滚；C/Rust 源 pre-split 失败 5/6 走有序重试；
  Rust 在复制一个 leaf 之后 `DestinationMapping::drop` 实现精确回滚。

### 换出源上的 DONTUNMAP（`smoke-swap`）

`swap_test` 先用 loop 设备建好 swap 区，再用 `madvise(MADV_PAGEOUT)` 把 4 个匿名页确定性换出，
然后对同一区间做 `MREMAP_DONTUNMAP`。断言的是：

- 换出后 `/proc/swaps` 的 in-use 恰好增加 4 页，`mincore` 报告这 4 页不在内存；
- 搬移后 in-use 数不变——目标只保留一份引用，源释放自己那一份；
- 读源地址得到零页，且 in-use 数不变（源 refault 不会复活旧槽）；
- 读目标得到逐字节一致的数据，in-use 数回到换出前的水平（换入释放全部槽）；
- 两次 unmap 之后 in-use 仍等于基线，没有槽泄漏。

这是确定性用例，不是概率性压力：区间、页数和换出动作都由测试驱动。

### 共享文件的注入失败路径（`smoke-mm-dontunmap-shared-rollback`）

同一套注入（`CONFIG_MM_DONTUNMAP_TEST_FAIL_AFTER_MAP_COUNT=1`）跑在 `MAP_SHARED` 文件映射上。
共享文件路径每个 leaf 都持有页缓存引用并在提交时收割 dirty 位，所以回滚除了清除目标，还必须
把这些引用还回去。断言：注入确实发生（`MM_DONTUNMAP_TEST: injected-failure leaves=1`）、
源字节与写权限保留、写回后宿主机侧文件内容与源一致、`MAP_FIXED_NOREPLACE` 探针证明目标已释放、
同进程重试成功且目标带上重试前写的字节、页缓存 pin 数回到基线。

### DONTUNMAP 与并发读者（`smoke-mm-dontunmap-race`）

8 核 guest 上，3 个读者线程反复读主线程正用 `MREMAP_DONTUNMAP` 在两个保留地址之间搬运的
8 页区间。读者只在“已填充”窗口内被放行，窗口关闭后主线程才搬移；读者的一次观测只有在窗口
开始与结束都成立、且区间指针未变时才计入。断言：搬移后的目标逐字节一致、被清空的源按零页
refault、读者观测到的每个字节都等于填充值、读者通过次数非零、RSS 不增长。

## 活 PTE 的 TLB 拆除（`smoke-mm-tlb-teardown`）

8 核 guest 上，3 个读者线程反复读**主线程正在 unmap 的同一批页**。读者只在“已填充”窗口内被
放行，窗口关闭后主线程才 `munmap`；紧随其后主线程把刚释放的帧回收进另一个保留区间并写满毒值
`0xEE`。于是：

- 读者赢了竞争，就必须读到该页被填充的字节；
- 读者输了竞争，就落在空洞上并缺页（计数，不是错误）；
- **帧在 shootdown 完成前被释放并复用**——旧错误——会让还在使用旧 TLB 项的读者读到毒值。

每轮还用 `MAP_FIXED_NOREPLACE` 探针证明“已释放”就是“不再映射”，并要求读者确实发生过缺页竞争、
RSS 没有增长，因此一条绿灯不可能来自“线程从未真正相撞”。

负控已实际执行：临时去掉 `mm_tlb_invalidate_finish()` 里的远端 shootdown 后重跑，读者观测到
`first_byte=0xee` 共 573 次，该门禁转红；恢复后重新通过。负控改动未提交。

## 分配失败的逐阶段回滚（`smoke-mm-alloc-fail`）

`CONFIG_MM_FAIL_STAGE=<n>` 每个镜像只让一个分配阶段失败一次，注入返回的是那个分配器本来会返回的
失败值（NULL / `PFN_NONE` / `-ENOMEM`），不跳过操作本身：

- 阶段 1：页表中间节点（`pt_walk()`/`pt_map_huge()`）；DONTUNMAP 目标 leaf 映射以 `-ENOMEM` 失败。
- 阶段 2：`mm_demote_huge_page()` 逐页复制的帧；大页部分 unmap 在已分配一半之后失败。
- 阶段 3：`swap_out_task_pages()` 的 swap 槽分配；`MADV_PAGEOUT` 必须留下全部页驻留、不消耗槽。

每个阶段都断言：操作以预期 errno 失败、`/proc/meminfo` 的 free 回到基线（允许 order-0 每 CPU
批量本身带来的 1 页偏差，负控泄漏是 256 页，不会混淆）、RSS 精确回到基线、关机时 `[MM-ASM]` 审计
全零——最后一条是必要的，否则一个把状态侧车指向已释放页的回滚仍会通过。

未注入的阶段：`pt_map_huge` 的中间节点（已接线但没有负载走到）、swap-in 路径
`handle_swap_fault`、THP 的 PMD 阶帧分配（其失败是静默退回 4K，没有可观测的 errno 回滚）。

## 共享写的冷启动与磁盘一致性（`smoke-mm-shared-write-coldboot`）

两次启动同一张 ext4 镜像：第一次在 guest 里对文件做 `MAP_SHARED` 写入并覆盖三种形态
（写后 `mprotect(PROT_READ)`、写后 `munmap`、映射在关机时仍然活着），然后 `sync` + `poweroff`；
宿主机对镜像跑 `e2fsck -fn`；第二次冷启动后逐字节读回比对。写入路径**不调用 `msync`**——
要证明的是 shutdown 路径自己保留了 dirty 状态。riscv64 与 aarch64 都通过，且负控（把比对期望
改成异或 0xA5）能让门禁转红。

顺带确认的契约：`poweroff` 本身就会刷盘（`sys_reboot` 先走 `vfs_system_shutdown()`），所以即使
去掉显式 `sync` 这门禁也仍然成立——`sync` 在这里只是冗余保险，不是证据来源。

## 宿主侧行为断言

`check-a20-tests` 里与 MM 相关的宿主用例（`test_usercopy_strings_host`、`test_usercopy_nommu_host`、
`test_usercopy_mapping_races`、`test_tlb_transaction_races`、`test_process_vm_host`、
`test_vma_region_host`）已经与当前实现对齐：它们驱动的是现在真正在跑的 Rust 循环与 Rust 合并策略，
保留原有行为断言与负控。`test_vma_region_host` 的 C 参考实现已被 Rust 取代，用例现在对齐的是
文档化的合并契约，而不是一个已经不存在的 C 函数。

## 覆盖范围与未覆盖项

RV64 Linux 是主线（`smoke-mm-*` 默认档，含 SMP2 与 SMP8 变体）；aarch64 覆盖共享写冷启动与
`ABI=both` 的 DONTUNMAP 聚焦用例。仍开放：32 位内核 bring-up（`kernel/mm/**` 已经全部编译通过，
剩余失败在 proc/net/fs，见 [mm.md](../gaps/mm.md) MM-05）、并发 VMO ref/attach/final-release 压力、
以及忠实的 C 性能对照。QEMU 结果不构成真机验证。

单一 `MM_STRESS` 绿灯不等于 MM 完成：上面每一条结论都只对应它自己那条门禁的断言。
