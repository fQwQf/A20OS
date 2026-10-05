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

## 5. vcpu 运行循环（已落地，`9858550b8`）

"真正跑指令"这一片已落地并过了门禁（`make smoke-hyp-vcpu`）。落地形状与当初
设想有一处实质改动：**guest trap 不再走独立向量与独立状态，而是复用内核标准
trap 路径，trap 帧本身就是 guest 状态**。当初提的 KVM-riscv sscratch 双区方案
因此没有采用。

### 5.1 运行形状

- **进入**（`kernel/arch/riscv64/hyp/hyp_vcpu_asm.S:89`）：先把宿主被调用者保存
  寄存器按冻结构造的偏移写进 `vcpu->arch[]`（`ra sp gp tp t0-t6 s0-s11`），
  发布 `g_hyp_arch_vcpu`，在内核栈上**清零并填出一个 70 槽标准 trap 帧**，
  `csrw stvec` 指向 `hyp_guest_trap_entry`，最后 `sret`（`hstatus.SPV=1`、
  `sstatus.SPP=1`）进 VS-mode。
- **guest trap 前导**（同上 `:66`）：VS-mode 与 HS-mode 共享物理寄存器组，从
  guest 陷出来时 `sp`/`tp` 是 guest 的，而 `__trap_from_kernel` 在任何 C 运行
  之前就通过 `tp` 存 sp guard 并在当前栈上建帧。前导把 guest 的 `sp`/`tp` 存进
  `vcpu->regs[2]`/`regs[4]`，切回宿主栈与宿主 `tp`，再 `j __trap_from_kernel`。
- **分类**（`kernel/core/trap.c:608`）：`kernel_trap_handler()` 在往帧里写任何
  宿主侧状态（x[0] 存地址空间 token）之前先问 `hyp_arch_guest_trap(ctx)`，答案为
  1 就直接返回，由 trap 返回路径把 guest 恢复。判定条件是"有活跃 vcpu 且
  `hstatus.SPV` 为 1"（`hyp_arch.c:246`）。
- **帧即状态 / 唯一接缝**：`hyp_vcpu_handle_trap()`（`kernel/hyp/hyp_vcpu.c:246`）
  只改 `vcpu->regs[]`/`vcpu->pc`，从不碰交给它的 trap 帧；架构半在返回 1 时把
  寄存器写回帧（`hyp_arch.c:286`）再走标准 trap 返回，在返回 0 时从 `arch[]`
  恢复宿主上下文。
- **退出**（`hyp_vcpu_asm.S:233`）：清掉 guest 侧的 hstatus 位、把 `stvec`
  换回 `__trap_from_kernel`、清 `g_hyp_arch_vcpu`，从 `arch[]` 装回宿主寄存器
  并 `ret` —— 直接回到 `hyp_arch_vcpu_enter()` 的调用点，整条 guest 帧连同它
  下面的 C trap 路径一起被 `arch[1]`（宿主 sp）丢弃。
- **宿主中断**：guest 运行期间来的是宿主 IRQ，它照样走同一个入口，在
  `hyp_arch.c:281` 对着 guest 帧跑完宿主 IRQ 机器，然后直接 `sret` 回 guest。
  宿主任务被抢占只是一次围绕栈帧的上下文切换。

三处支撑改动：

- `trap.S` 的帧释放从 `addi sp, sp, 70*8` 改成 `ld x2, 2*8(sp)`（`:467`）——
  x[2] 是 trap 返回要恢复的 sp，帧其余槽都由 sp 寻址，sp 只能最后动。同一处
  把帧的 x[2] 改成"trap 发生时的 sp 本身"（`:383-385`），此前存的
  `sp + 70*8` 会让被打断的代码每一条相对 sp 的偏移都偏 560 字节。
- `kernel/hyp/hyp.c:198` 的 stage-2 叶**加上 `PTE_U`**：G-stage 走查本身以 U
  权限运行，没有 U 的叶会在看 R/W/X 之前就被拒（QEMU `cpu_helper.c` 的
  "supervisor PTE flags when not S mode"）。v1 半原来剥掉 U 是反的。
- Linux ABI 桥 `kernel/abi/linux/sys_a20_bridge.c` 用 pid 打标签的槽表暴露同一
  批内核对象，`proc_exit()` 调 `hyp_bridge_task_exit()` 回收（`exit.c:446`）——
  pid 会回收，不回收就等于后来的任务能装进、跑起并拆掉一个已死任务的 VM。

### 5.2 验证

`make smoke-hyp-vcpu`：`hyp_test`（`user/cmds/core/hyp_test.c`）建 VM、装 56 字节
手写 RISC-V 机器码、跑 guest，断言退出原因是 `HYP_EXIT_SHUTDOWN`，并且 console
上必须出现 guest 自己打出的独立一行 `HYP`（`tools/smoke_cases.py:533` 用
`^HYP$` 锚定，裸 `HYP` 已被 `HYP_VCPU_TEST` 包含而不成立）。同一条 gate 必须带
`-cpu rv64,h=true`：默认 rv64 CPU 不暴露 H，`hyp_supported()` 会让所有调用
SKIP，gate 就变成"过了但什么都没测"。

### 5.3 评审发现与遗留

均为本片读码得出的结论；本片只写文档，没有改任何代码。

1. **`trap.S:461-466` 的注释与实现不符**：它说 guest 跑在一棵"带着宿主子树的
   自己的 stage-1 根"上、进出各有一次 satp 切换。实际是 vsatp 停在 MODE=Bare、
   satp 全程不动（`hyp_arch.c:184-193`、`hyp_vcpu_asm.S:200-205`），trap 返回时
   硬件自己完成 satp 与 vsatp 的暂存/恢复。注释是早期形状的残留。
2. **只在 TCG 上验证过**：`hedeleg`/`hideleg` 全写 0（`hyp_arch.c:168-169`），
   这一片只在 QEMU 10.0.13（Debian 1:10.0.13+ds-0+deb13u1）的
   `-cpu rv64,h=true` 下跑过，没有在真实实现 H 的 CPU 上验证过任何行为。
3. ~~**guest RAM 页面是 R|X，不是 R|W**~~ —— **已推翻并修正。** 原结论说 W 与 X
   同时置位在叶 PTE 里是保留组合、G-stage 走查直接拒，**不成立**：QEMU
   `get_physical_address()` 的叶处理只特判 `rwx==6`（W|X）与 `rwx==2`（W），
   `rwx==7`（R|W|X）落到 `PAGE_READ|PAGE_WRITE|PAGE_EXEC`，正常翻译。
   工作树现在按 `HYP_RAM_PAGE_PROT (PTE_R|PTE_W|PTE_X)` 装 guest RAM
   （`kernel/hyp/hyp.c:50`），`hyp_vm_load()` 装进去的镜像用同一个值
   （`kernel/hyp/hyp_vcpu.c:91` 的 `HYP_GUEST_PAGE_PROT`）。所以"guest 代码与
   数据不能同页、镜像不能带栈"这个后果不存在——A20OS 的 `.bss` 与其中的
   `_stack_end` 正需要 R|W。
4. **句柄类型没进对象枚举**：Native ABI 的 VM/vcpu 用私有 type 18/19
   （`sys_native_hyp.c:53-54`），`a20_type_valid_rights()` 对其返回 0，于是
   handle 表的 ref/release 臂是空操作，vcpu 持有的 VM 引用永远不会被 drop；
   Native ABI 也没有 vcpu-destroy。两条缺口在 `sys_native_hyp.c` 文件头里已经
   写明。
5. **单 guest 槽是全局的**：`hyp_active_vcpu` + `spin_trylock_irqsave`
   （`hyp_vcpu.c:74-88,311-322`）。claim 用的必须是真跨 CPU 锁而不是
   `arch_local_irq_disable()`（后者只关本 CPU 的窗）；用 trylock 而非等锁，
   是因为输了竞争要回 `-EBUSY`，而不是举着锁穿进 guest 执行。
6. **`trap.S:434` 与 `:443` 的缩进**在该提交里被压到了行首，是格式回退。

### 5.4 下一片：v2

把 A20OS 自己当 guest：委托式 trap、按需 RAM 窗口、16550/CLINT 设备模型、
最小 FDT 引导。行为契约在 `kernel/include/hyp/hyp_vcpu.h` 的 v2 段，设计、
评审发现与冒烟判据见 [01-a20os-guest.md](01-a20os-guest.md)，后续切片见
[02-roadmap.md](02-roadmap.md)。

## 6. 验证入口汇总

| 内容 | 入口 |
| --- | --- |
| stage-2/出借/审计自测 | `make smoke-hyp-selftest` |
| vcpu 运行循环端到端 | `make smoke-hyp-vcpu` |
| 大页叶建模 | `make smoke-mm-stress`（huge_install>=1 + 审计全零） |
| 无锁 COW | 同上（cow_from_status>=1）+ `make check-mm-pt-lock-order`（32 条） |
| 锁模型回归 | `make check-mm-lock-model`（14 条） |
