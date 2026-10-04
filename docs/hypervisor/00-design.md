# A20OS hypervisor foundation（设计记录）

本文记录 hypervisor/host 侧的地基设计与已落地范围。这不是"要不要做"的论证
（见 docs/roadmap 的取舍），而是在单级内存模型迁移完成后，地基应该长什么样。

## 0. 前提：三个内存模型问题已经解决

hypervisor 最吃内存模型。在动 stage-2 之前，`feat/virt-foundation` 分支先
解决了三个迁移遗留（各自独立提交）：

1. **大页叶级建模**（`b0359ff02`）：`pt_map_huge()` 从不写 status、fork 的
   父侧 COW 同步止步 level 0、`cow_sync_status()` 对大叶读错槽位。更严重的
   是实测发现 THP 路径从未执行过：同一个 2 MiB 块里任何早先 4K fault 留下的
   **空** level-0 表都以 -EEXIST 永久挡住大叶（实测 live-children=0）。现在
   空表在父→子节点锁下退役后装入大叶；`smoke-mm-stress` 断言
   `huge_install>=1` 与审计全零。
2. **锁模型推广**（`a455e3d3d`）：无锁 COW 切片 `mm_cow_from_status`——
   rc>1 的共享匿名 COW 缺页只凭叶锁装私有副本；fork 的父侧重写、mprotect
   的 PTE 重写与 status 刷新都纳入同一把节点锁并带锁下重查。首个版本踩了
   两个运行期坑：pfa.lock 临界区内调 `frame_get()`（自死锁）和用可回收分配
   器（reclaim 拿节点锁，LOCK-STALL）。`check-mm-pt-lock-order` 新增 5 条
   断言钉住这个锁三角。
3. **guest 状态格式**（本文 S2/S3）：`FRAME_F_GUEST` 帧标志与
   `MM_ST_GUEST_MEM` stage-2 叶类。

## 1. 定位

A20OS 的 hypervisor 不是 KVM 的复刻。它是 capability 叙事下的最小内核侧
地基：stage-2 是"又一个地址空间"，其并发、审计、生命周期全部复用单级模型；
guest RAM 是被出借（lend）的帧，出借是身份标志 + 引用计数，不是新记账系统。

优先架构是 riscv64（H-extension，QEMU TCG 可测）。arm32/loongarch 无
stage-2 硬件的架构走 stub（`hyp_supported()==0`），generic 半在任何架构都
编译。

## 2. stage-2 = 单级模型的一个新客户

`kernel/hyp/hyp.c`。决策与依据：

- **stage-2 页表页挂 `pt_meta_t`**：同型 radix 树，节点 MCS 锁、逐条目
  status 字节全部原样复用。并发故事（cursor/叶锁/RCU 退役）对 guest 物理
  地址空间照搬，不需要第二套理论。
- **叶类 `MM_ST_GUEST_MEM`**：stage-2 叶的"后备是什么"由 PTE 回答（宿主
  帧），宿主侧的 anon/file/vmo 类在这里都是谎话。宿主审计把 GUEST_MEM 记为
  mismatch，因此 stage-2 字节漏进宿主表会被抓。
- **`FRAME_F_GUEST`**（`kernel/include/mm/frame.h`）：帧被出借的身份标志。
  引用计数本就保证帧活着；标志给回收扫描一个免遍历的跳过依据，给审计一个
  "VM 死了没还帧就藏不住"的交叉检查（`mm_s2_audit` 走两遍树：status↔PTE、
  标志↔映射）。
- **GPA 上限 512 GiB**：hgatp 是 Sv39x4，但本实现只编程根表 [0,512) 条目，
  根表保持 512 条目的常规 `pt_meta_t`。解除限制需要 x4 根（2048 条目）与
  更宽的根元数据块——那是 `pt_meta_t` 的格式变更，按 P7 的纪律刻意缓行。
- **中间节点用 `frame_alloc_nr()`**（不可回收分配器）：map 持父节点锁时装
  子表，可回收分配会到 oom_try_reclaim 拿节点锁——与宿主 cursor 同一条
  已确认的死锁路径。
- **VMID 只增不重用**：guest TLB 标签，重用 = 陈旧翻译可能越过 fence。
  代价是每 VM 一次 hfence，收益是没有 alias 类 bug。宽度是 WARL，环绕是
  文档化限制。

## 3. 架构半（riscv64）

`kernel/arch/riscv64/hyp/hyp_arch.c`。三个入口：`hyp_supported()`（读
`hstatus`——它只在 H 实现时存在，读它本身就是探测；`csrr misa` 曾先被试用，
在 QEMU rv64 下触发非法指令，原因未查清，规格说 misa 任意特权可读，记录在
案）、`hyp_arch_vmid_fenced()`（VMID 首用钩子）、`hyp_arch_s2_fence()`
（`hfence.gvma`，经 `.insn` 手工编码——树内 `-march` 不带 `h`，汇编器拒绝
助记符；`.insn` 是指令而非助记符，不需要扩展位）。

## 4. 内核侧自测（已过门禁）

`/proc/a20/hyp_selftest`（读即运行，netfilter 式控制文件），步骤：create →
map 4 页（写入模式、经 stage-2 翻译读回）→ lend 标志 → `mm_s2_audit` 全零 →
unmap（翻译消失、帧归还、标志清除）→ remap/-EEXIST → destroy（四帧全还）。
`smoke-hyp-selftest` 在 QEMU `-cpu rv64,h=true` 下断言 console 的
`HYP_SELFTEST: PASS` 与读回的 `hyp_selftest=PASS`。默认 rv64 CPU 不暴露 H
扩展，不带该参数时自测 SKIP——那会让门禁"过了但什么都没测"，参数是门禁的
承重件。

## 5. 下一片：vcpu 运行循环（未落地）

这是 hypervisor 真正"跑指令"的部分，刻意独立成片：

- **上下文切换形状**：host 线程陷在 syscall 里，enter 例程把宿主被调者
  保存寄存器存入 vcpu，装入 guest GPR，`sret`（`hstatus.SPV=1`）进 VS-mode；
  guest trap 落到内核 `stvec`，trap 入口必须在污染宿主状态**之前**识别
  guest trap（`hstatus.SPV`），把寄存器存进 vcpu 的 guest 保存区，再恢复
  宿主 C 上下文返回运行循环。KVM-riscv 的 sscratch 双区方案是参照。
- **最小拦截集**：VS ecall（legacy SBI console_putchar/shutdown）、
  二级缺页（`htval`+`stval` → `hyp_s2_map` 补表）、WFI（vcpu 睡眠）。
- **用户 API**：native ABI 上的 vm_create/vcpu_run（handle 语义天然贴合：
  VM 是对象，vcpu 是它的 handle）。
- **最小 guest**：由用户态测试程序直接往 guest RAM 写入字节码（SBI putchar
  循环 + shutdown ecall），不引入 guest 镜像构建。

## 6. 验证入口汇总

| 内容 | 入口 |
| --- | --- |
| stage-2/出借/审计自测 | `make smoke-hyp-selftest` |
| 大页叶建模 | `make smoke-mm-stress`（huge_install>=1 + 审计全零） |
| 无锁 COW | 同上（cow_from_status>=1）+ `make check-mm-pt-lock-order`（32 条） |
| 锁模型回归 | `make check-mm-lock-model`（14 条） |
