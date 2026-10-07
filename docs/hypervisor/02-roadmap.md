# hypervisor 路线图

最后核实：本文记录的是 v2（`01-a20os-guest.md`）之后的切片顺序与各自的
前置条件。**§0–§6 没有任何一整项已落地**，写在这里是为了说明"为什么是这个顺序"
以及每一项被什么卡着。已落地的部分见
[00-design.md](00-design.md)，v2 的设计见 [01-a20os-guest.md](01-a20os-guest.md)。

**但其中几条子项的结论在本轮被推翻了，不要按"整项未落地"顺推子项状态**：

- **【已落地】§2.2 的 guest 时钟中断路径已经通了**，而且是硬件行为不需要 HS 注入（依据表见
  §2.2）。**但"通了"只到"装得上定时器"为止**：至今没有一次运行观测到 guest
  真的收到并处理了它的 tick（`03-usage.md` §4.5 的 (b) 段末尾写了这条限制）。
  §2 剩下的是外部中断注入与多 vcpu。
- **【已落地】§4.1 的"没有块设备就进不了 guest shell"只对 `RAMFS_USER=0` 的镜像成立**；
  `RAMFS_USER=1` 的 guest 的 `/bin/init` 就在内核 ramfs 里（依据见 §4.1）。
- **【已落地】本轮另落地了一条 §0–§6 之外的通道：guest 控制台输入**（契约 v3，见
  [03-usage.md §3.4](03-usage.md)）。它**不走 §2 的中断注入**：纯轮询 `LSR`/`RBR`，
  这正是 §2.2 缺口那句话的一个反例——"要注入才有用"只对中断驱动成立。往返已在
  `smoke-hyp-console-p0` 上实测通（该门禁日志里 guest 把三个字节回显成独立一行
  `HYP`），**这条门禁当前是绿的**：它一度红过，红的不是通道而是 guest 回显之后的
  `pc=0` 取指失败，成因是 `hyp_test.c` 把 `memcpy` 的元素个数当成了字节数，已修
  （详见 §3.4.1）。

- **【未落地】但"设备模型这一段通了"不等于"A20OS-as-guest 能交互"。** 往返只在
  `smoke-hyp-console-p0` 的**合成 guest**（13 条指令的 `hyp_test echo`）上被证明。
  真 guest 那条门禁 `smoke-hyp-console` 的 `rx_bytes=15` 非零，却**一次 RBR 都没读**
  （`grep -c 'scause=15.*stval=ffffffc010000000'` = 0，同一偏移上的 10 次全是
  `scause=17` 的 THR 写，另有 11 次 LSR 读），因为它在
  `[RAMFS] Initialized` 之后死于自己的 `kfree`，从没进到 shell
  （[03-usage.md §3.3、§3.4](03-usage.md)）。这一条**不靠 §2 的中断注入，也不靠
  §4 的 virtio**。
- **本轮为此新增了一条门禁 `smoke-hyp-shell`（`tools/smoke_cases.py:810-962`，
  `tools/targets-smoke.mk:202-213`），它是"guest 能进 shell 并且能交互"这条目标的
  验收点，当前是红的。** 它要求真 `RAMFS_USER=1` guest 走到 mksh 提示符、执行门禁
  敲进去的 `echo AAAABBBBCCCC` / `echo DDEEEEEEFFFF`、把两个 token 打印回来、
  回到提示符、再由 `exit` 收尾，最后 `HYPVM:` 行上的 `rx_bytes` 非零。
  **"字节进了环"与"guest 读了"是两条分开的断言**，因为上一轮只断言了前者。
  实跑结果：`FAIL`，缺 5 条期望、违反 2 条 forbid。
  **卡在哪（按本轮日志 `.kernel-build/smoke/hyp-shell-riscv64.log`）**：泵这一段是通的
  ——`rx_bytes=18`，正好是 `echo AAAABBBBCCCC\n` 的 18 个字节——但 guest 死在
  `[INIT] entering scheduler...` 上的**内核栈页 use-after-free**
  （`[TRAP] pfn 32638 sits on buddy free list — use-after-free of stack page`，
  `[PANIC] task: pid=1 name=kthread`），**连 `/bin/init` 都没 exec**。
  注意这一轮的失败点与上一轮**不同**：上一轮是 `[RAMFS] Initialized` 之后的
  `kfree` / `[SLAB BUG]`，本轮那两个字符串在日志里一次都没出现
  （`grep -c 'SLAB BUG'` = 0），说明 guest 确实走得更远了。
  **这一条同样不靠 §2 的中断注入、也不靠 §4 的 virtio**：拦路的是 guest 自己
  内核初始化路径上的栈页回收，本片没有定位根因。逐行解读见
  [03-usage.md §3.4.2](03-usage.md)。
- **顺手记一条给下一个人：guest 的 rootfs 就是 `RAMFS_USER_PROGRAMS` 那十来个程序
  （`Makefile:1238-1241`），里面没有 `poweroff`。** 所以"结束一次 guest 交互"只能
  敲 `exit`：mksh 退出后 `user/init.c:264-283` 的 `waitpid` 循环跳出、调
  `do_shutdown()`，后者打 `[init] shutting down` 再 `reboot(RB_POWER_OFF)`
  （`user/init.c:137-143`），宿主才把它接成 `HYP_EXIT_SHUTDOWN` 让 `hyp_vm_run()`
  返回。**不给树加东西就能收尾的唯一办法就是它。**

- **偷字节这条路今天有一个已实测的副作用**：用 `sendline`（一次性把命令排进
  stdin）跑 `hypvm` 时，紧跟着的 `poweroff` 会在 guest 运行期间被整行搬进 guest 的
  收字节环然后丢掉——`smoke-hyp-vm` / `smoke-hyp-vm-96` 的 `rx_bytes=9` 正好是
  `poweroff\n`。两条门禁因此是靠 300 s 超时被掐掉的（仍报 PASS，因为
  `tools/smoke.py` 的 `report()` 只看日志文本、不看 QEMU 的退出码）。本轮新写的两条
  console 门禁把收尾步骤挂在程序自己的 `PASS` 行上、并断言
  `System is going down for power-off NOW`；**这两条老门禁还没改，记在这里当欠账**。

顺序不是偏好，但也不是一条链。修正后的依赖关系：

- §1（CSR 规格化）与 §2（多 vcpu + 外部中断注入）互为前置——CSRs 要先定死，
  多 vcpu 要先把全局槽换成 per-CPU 发布。
- §3（零拷贝 VMO guest RAM）与 §4（virtio 设备模型）之间的依赖比原先写的弱：
  §4.2 第 2 条给出 virtio-blk 的纯轮询完成路径（`kernel/drivers/block/
  virtio_blk.c:749-763`），所以 §4 不必等 §2 的中断注入；但 §4 的 DMA 确实要
  §3 之后才有着落点（guest 内存的身份与生命周期要有确定答案）。
- §5（x4 根表）与 §6（真机验证）与上面正交，可任意时点插入。

---

## 0. 依赖关系一览

```
v2 已定义的委托/设备/引导面
        │
        ├─ §1 hstatus/hcounteren 规格化 ─┐
        ├─ §2 多 vcpu + 外部中断注入 ────┤（需要 §1 的 CSR 规格先定死）
        │                                │
        │        ┌───────────────────────┘
        │        ↓
        │   §3 零拷贝 VMO guest RAM（capability 叙事的正主）
        │        │
        │        └─→ §4 guest virtio 设备模型（真 rootfs）
        │
        └─ §5 x4 根表 / §6 真机验证：与上面正交，可在任意时点插入

     §2 的中断注入 ──✗─→ §4：两者无依赖（virtio-blk 有 poll-only 完成路径）
     §4.1 的 RAMFS_USER=1 guest ──✗→ §4：那条 guest 根本不需要块设备
```

---

## 0.1 从 KVM 借鉴了什么，以及明确不借鉴什么

`01-a20os-guest.md` §9 列的是"与 KVM 的设计差异"。差异表回答"我们不一样"，
本节回答"**我们抄了 KVM 的哪一部分形状，以及哪些是明确决定不抄的**"——后者更重要，
因为不抄的东西如果没有写下理由，下一个人会以为那只是还没来得及做。

### 借鉴的（有代码或契约对应）

| KVM 的做法 | 本树的对应物 | 依据 |
| --- | --- | --- |
| guest trap 借宿主 trap 路径，`sscratch` + per-vcpu trampoline bank | 同样借标准 trap 路径，`vcpu->tramp[]` 是那段 bank，汇编可见偏移钉在 `HYP_VCPU_TRAMP_U64` | `kernel/include/hyp/hyp_vcpu.h:46-52`、`kernel/arch/riscv64/hyp/hyp_asm_offsets.h` |
| 宿主页表机制（Guest memory slots、缺页回调、map/unmap 按页） | stage-2 页表 + `hyp_ram_fill()` 按页补零 + `mm_s2_audit()` 双向审计 | `kernel/hyp/hyp.c`；`kernel/hyp/hyp_vcpu.c:186-259` |
| vcpu run loop 是一个"进 guest / 陷出来 / 分类 / sret 回去"的循环 | 同款四段，分类器是 `hyp_arch_guest_trap()` 与 `hyp_vcpu_handle_trap()` | `kernel/hyp/hyp_vcpu.c:1046-1150`、`hyp_vcpu_asm.S` |
| "trap 帧就是 guest 状态，不必另存一份" | 同样：`hyp_vcpu_handle_trap()` 只改 `vcpu->regs[]`/`pc`，从不碰交给它的帧 | `hyp_vcpu.c:1046-1049`（`(void)trap_frame;`） |
| guest 的 vCPU 创建要有 canary / magic 防止野指针 | `HYP_VM_MAGIC` / `HYP_VCPU_MAGIC`，每次入口都查 | `hyp_vcpu.c:1051,1159-1162` |

**一条借鉴的形状值得单独说**：最初的设计提过"KVM-riscv 的 sscratch 双区方案"，
落地时**没有采用**——guest trap 走的是内核自己的标准 trap 路径，帧本身就是 guest
状态（`00-design.md` §5 的"帧即状态 / 唯一接缝"）。所以这里借鉴的是 KVM 的**接口
形状**，不是它的**实现位置**：KVM 把 guest 的 trap 状态放在自己的数据结构里，
本树让 guest 状态住在内核的 trap 帧里。代价是宿主 IRQ 必须在 guest 帧上就地跑完
再 `sret`（`hyp_vcpu.c:1074-1077` 的即时返回），收益是不需要第二套寄存器保存代码。

### 明确**不**借鉴的，以及为什么

| 不借鉴 | 理由 |
| --- | --- |
| **用户态设备模型**（KVM 把 virtio / PCI 整个放在 QEMU 里） | 本树的 guest 帧要过 `mm_s2_audit()` 的双向交叉检查（status↔PTE、标志↔映射）。设备要往 guest 内存写 DMA，目标地址必须能被 stage-2 翻译并且能被审计；这条约束在用户态就没有对应物（`01-a20os-guest.md` §9 的"核心差别只有一条"） |
| **memslot / `KVM_SET_USER_MEMORY_REGION` 式的地址空间登记** | 同上。guest RAM 是一组被出借的帧（`FRAME_F_GUEST` 身份标志 + 引用计数），不是一个由外部登记的地址区间。零拷贝（§3）是把同一个语义推到 VMO 级，不是把 memslot 搬进来 |
| **vCPU 是可抢占线程，有独立调度类与定时器** | 全局单 guest 槽 + `spin_trylock_irqsave()`，输了竞争回 `-EBUSY`（`hyp_vcpu.c:1181-1184`）。多 vcpu 是 §2，是一次显式的契约变更（`g_hyp_arch_vcpu` 与 `arch[]` 的归属要重新定），不是"照搬 KVM 的调度器" |
| **注入式时器中断**（KVM 在 vcpu 定时器到点时置 `hvip.VSTIP`） | 在 H 扩展 + Sstc 上这是多余的一次写：vstimecmp 到期由硬件置 `mip.VSTIP`，`hideleg` bit 6 已委托（§2.2 的依据表）。这正是"借鉴"与"照抄"分界的地方——KVM 需要注入是因为它没有 sstc 的 guest 面 |
| **用户态 /dev/kvm ioctl 接口 + QEMU 的 vCPU 抽象** | 本树的机器形状收进内核（`01-a20os-guest.md` §9）。用户入口是 Native ABI 的几个调用加上 Linux ABI 桥 |
| **guest 内存的页保护分级**（KVM 允许按 slot 标 RWX） | 本设计一次只跑一个 guest，窗口页一律 RWX（`01-a20os-guest.md` §10 第 3 条）。等 guest 之间的隔离真的成为对象时再分级 |

**一句话**：我们借鉴 KVM 的是**接口形状与那几条已经被反复验证过的循环结构**；
不借鉴的是**"机器的形状放在用户态"这个前提**，以及它带来的两个后果（用户态设备
模型、memslot 记账）。这两条在 `01-a20os-guest.md` §9 里已经写成表格，本节补的是
**为什么不做**——因为 stage-2 页表在单级内存模型里是一等公民，拆到用户态就失去
审计。

## 1. HS-mode CSR 规格化（`hstatus` / `hcounteren` / `htimedelta`）

**状态：未落地。** v2 只在契约里钉了两个值（`VTVM`、`hcounteren.TM` 应当置位），
完整的清单还没写下来。

**为什么先做它**：v2 的整个思路是"HS 只在二级缺页和设备 MMIO 两个地方动手"。
这条思路能不能成立，取决于哪些 HS-mode CSR **必须**被编程才能让 guest 正常跑。
这个清单现在散在规格里和代码注释里，没有一处是完整的，而每一项漏掉的后果都是
guest 在某个点莫名死掉，很难定位。

**要定死的东西**：

| CSR / 位 | 谁需要它 | 不配的后果 |
| --- | --- | --- |
| `hstatus.VTVM` = 0 | 契约的 VS CSR 直通（[01 §3](01-a20os-guest.md)） | guest 的 `csrw satp` 变成 virtual-instruction exception |
| `hcounteren.TM` = 1 | guest 的 `timer_get_ticks()` 读 `time` | 低特权级读 `time` 触发异常 |
| `htimedelta` = 0 | 与宿主共钟 | 无（这是默认值，写下来是为了不再有人动它） |
| `hstatus.VTSR` / `VTW` | guest 会执行 `sret`（进出用户态）与 `wfi`（空闲） | 虚拟指令异常，停在半路 |
| `hvip.VSEIP` / `VSSIP` | §2 的外部与软件中断注入 | guest 的外设中断与 IPI 永远等不到 |
| `hstatus.VGEIN` / `hgeip` / `hgeie` | §2 的 VSEIP 转发 | 读 CSR 得到未知值或全零 |
| `hgatp` 的 MODE/VMID/PPN 布局 | G-stage 走查 | 见 `00-design.md` §3 的 `hyp_make_hgatp()` |

**一句需要纠正的话**：`hstatus` 里**没有**中断使能字段。QEMU 的
`target/riscv/cpu_bits.h:612-623` 只定义了 VSBE/GVA/SPV/SPVP/HU/VGEIN/VTVM/VTW/
VTSR/HUKTE/VSXL/HUPMM——**bit 5 是 VSBE，bit 10 没有对应字段**。所以
**这台平台上"HS 侧的 guest 中断使能"不是 hstatus 的字段**，那一类问题不能靠改
hstatus 解决，必须走 §2 的"HS 处理完自己的中断后用 `hvip` 断言 guest 中断"这条
路。把这件事写进规格文档，是为了让下一个读代码的人不必重新推导一遍。

不过要说清楚当前代码的实际状态，免得读者按本文去找一句并不存在的注释：工作树
**确实把 bit 10 编进了 `HYP_HSTATUS_GUEST_ON`**——`kernel/arch/riscv64/hyp/
hyp_arch.c:166-167`（C）与 `kernel/arch/riscv64/hyp/hyp_vcpu_asm.S:47-53`（.S）
各一份。这是一次**空操作**而不是一次配置：QEMU 没有实现该字段，写它等于写
`hstatus.WPRI` 的保留位。它是为了满足规格语义而留着，不是"打开"了什么；真正让
guest 中断到达 VS-mode 的是 QEMU 在 V=1 时无条件置的 `hsie`（见
`hyp_arch.c` 里 `hyp_arch_vcpu_enter` 的同名注释）。本文此前引用"该字段从来没被
编程过"的那句注释已随本次修订删除——它与代码不符。

**前置**：无。**产出**：一份 HS-mode CSR 清单（放 `kernel/include/hyp/hyp_arch.h`
或本文的姐妹篇），以及 `hyp_arch_vcpu_setup()` 里对应的赋值。

---

## 2. SMP：多 vcpu 与中断虚拟化

**状态：未落地。** v1/v2 都是全局单 guest 槽
（`kernel/hyp/hyp_vcpu.c:1157-1190`）。

### 2.1 阻塞项

单 guest 槽不是实现偷懒，是被逼的：`hyp_active_vcpu` 是**一个指针**，
`g_hyp_arch_vcpu` 也是，汇编前导靠后者在没有任何 C 运行的情况下找到 vcpu
（`hyp_vcpu_asm.S:71-77`）。多 vcpu 要先把这两处换成 per-CPU 的发布结构，
或者引入 `hsm` 意义上的 vcpu id —— 后者是 ABI 变更，契约 v2 段刚冻结，得走一次
契约变更流程。

更硬的一条：`arch[]`（`arch[0..22]`）是**宿主**被调用者保存寄存器，存放在 vcpu
对象里而不是 per-CPU 区。多 vcpu 时每个 vcpu 的 enter 会覆盖自己那份，没问题；
但"哪个 vcpu 现在在这颗 CPU 上"这件事必须 per-CPU，且汇编前导必须在无 C 的
情况下读到它。

### 2.2 中断虚拟化（比多 vcpu 更卡）

**先更正本节此前的前提。** 原文说"guest 要有 tick，HS 就得能制造 guest 中断"，
并把规格给的三条路并列为待选项。这条前提是错的：**guest 的时钟中断已经通了，而且
不需要 HS 去"制造"它。**

**已经落地的（读码即可确认）**：

| 事实 | 依据 |
| --- | --- |
| `hideleg` 委托了 VS 级中断位 bit 2/6/10（VSSIP/VSTIP/VSEIP） | `kernel/include/hyp/hyp_vcpu.h:226-227`，写 CSR 在 `kernel/arch/riscv64/hyp/hyp_arch.c:284` |
| `hstatus.VIE` 随 `HYP_HSTATUS_GUEST_ON` 一起置位 | `hyp_arch.c:151,166-167,315`，汇编侧 `kernel/arch/riscv64/hyp/hyp_vcpu_asm.S:52-53` |
| vstimecmp 到期由 QEMU 置 `mip.VSTIP`，不是软件注入 | `target/riscv/time_helper.c:25-30`（QEMU 10.0.13）的 `riscv_vstimer_cb()`；挂定时器的是写 CSR 0x24d，`target/riscv/csr.c:1680-1692` |
| 被委托的 VS 位由硬件交给 VS-mode | `target/riscv/cpu_helper.c:585`（V=1 时 `hsie` 恒 1）与 `:617-634` |
| guest 侧两条入口都接上了 | Sstc：`kernel/hyp/hyp_vcpu.c:988-1003` 仿真 CSR 0x24d；legacy `set_timer`：`hyp_vcpu.c:386-388` 由分派器代写 |

所以规格里的**第 3 条路（"让 guest 的 vstimecmp 真的驱动物理 CLINT"）已经在走**，
QEMU 实现了 sstc 的 guest 面。原先写在这里的"本片未验证"可以划掉：验证方式是读
`target/riscv/time_helper.c` 与 `target/riscv/csr.c`，不是跑门禁。
**未验证的只剩一件**：还没有一次运行证明 guest 真的**收到并处理了**自己的定时器
中断——本轮实测只到"装上了"（guest 活着走完 `[INIT] Timer initialized` 之后的
内存子系统），`timer_preempt` 之类的 guest 侧证据要等 guest 活到有一个 tick 可观察
的地方。**注意别把"装得上"读成"收到了"**：本节标题里"已经通了"指的是委托位与使能位
在位、QEMU 会置 `mip.VSTIP`，不是已经观测到 guest 处理了中断。

**剩下的缺口是外部中断注入**，规格给的第 1、2 条路都还没有走：

1. **`hvip.VSEIP` / `VSSIP` 是可写的**——HS 直接置位即向 VS 断言虚拟中断。
   全树零引用（`grep -rn 'hvip\|hgeip\|hgeie' kernel/ user/cmds --include=*.c
   --include=*.h --include=*.S` 无输出）。
2. **`hgeip` / `hgeie` + `hstatus.VGEIN`**——把 guest 外部中断号映射到 VS 级外部
   中断。这是 PLIC 的正路。
3. ~~`hvip.VSTIP` / `VSSIP`~~ ——时器那一位不需要走 hvip：vstimecmp 到期这一条
   OR 来源已经在硬件里，另两条 hvip 位只是同一件事的另一种写法。

**配套缺的不只是 CSR 写入。** 设备模型里没有 PLIC：非 UART/CLINT 页一律 RAZ/WI
并按页计数（`kernel/hyp/hyp_dev.c:472-477`，`hyp_dev_note_unknown()` 在 `:100-112`），
UART 的 IIR 恒回"无中断待处理"（`hyp_dev.c:71-76,286`）。要注入一条外部中断，先得有一个"guest 认为已经发生"
的来源；否则注入的是一个 guest 从没设过 handler 的 cause。**所以这一项的顺序是
PLIC 模型在前、注入在后**，不是反过来。

**反过来的一条教训值得写在这里**：本轮落地的 guest 控制台输入（契约 v3）**一次
hvip 都没写**就通了，因为 guest 侧是轮询 `LSR` 的。也就是说 §2.2 的缺口**不是所有
设备的前置**——轮询驱动的设备可以在 §2 之前做，这正是 §4.2 第 2 条
（virtio-blk 的纯轮询完成路径）成立的原因。

### 2.3 客服用到的 SBI 扩展

guest 侧的 SMP 完全建立在 SBI 上（`kernel/platform/qemu-virt-riscv64/board.c`）：
`sbi_hart_start()` 启 secondary hart、`sbi_send_ipi()` 发中断、
`rv64_smp_remote_tlb_flush()` 自己用 IPI 实现远端 TLB 刷新（注释明确说 QEMU
TCG 下 `SBI REMOTE SFENCE.VMA` 不可靠）。这三项都不在 v2 的 SBI 面里。

**顺序建议**：先做 IPI（HS 用 `hvip.VSSIP` 断言 guest 软件中断），再做
hart_start（HS 直接往 guest RAM 写 secondary 的入口并置其 gate），最后才是
hstatus 的 `deleg` 与 CSR 读写。这三件事都不需要改契约。

---

## 3. 零拷贝 VMO guest RAM —— capability 叙事的正主

**状态：未落地。** 当前的 guest 帧由 `hyp_ram_fill()` 从帧分配器现取
（[01 §5](01-a20os-guest.md)），是**复制/新建**出来的，不共享任何宿主内存。

### 3.1 为什么这是正主

capability 叙事的核心主张是"资源以能力的形式出借，宿主与客户共享同一个对象，
而不是各持一份"。在内存这条线上，它应该长这样：

```
今天：  guest 页  =  hyp_ram_fill() 从 pfa 现分配的一帧      （guest 独占）
零拷贝：  guest 页  =  宿主 VMO 的某一页，出借进 stage-2      （宿主 VMO 与 guest 共用）
```

差别不是性能，是**身份**。今天 guest 用过的内存在 `pfa` 的统计里是"guest 的
内存"；零拷贝之后同一页在 VMO 的记账里是"某个进程的内存，只是被借给了某个 VM"。
cgroup 记账也才真正有意义——借出的页记在**持有 VMO 的那个任务的 cgroup** 上，
而 VM 自己没有预算身份，这与 §3.2 的记账改动是同一件事。

### 3.2 树里已经有的东西

- **出借机制已经是帧级的**：`FRAME_F_GUEST` 身份标志 + 引用计数，
  `mm_pt_frame_lend/return` 走同一条路（`kernel/include/hyp/hyp.h:22-24`）。
  零拷贝不是新机制，是把 `frame_alloc` 换成 `vmo_get_page()`。
- **记账接口是现成的**：`vmo_get_page_charged()`（`kernel/mm/vmo.c:152,242`）
  就是"按页物化并记到 cgroup"，`cg_mem_charge()` / `cg_mem_uncharge()`
  （`vmo.c:276,284`）就是本轮要用的两条。
- **审计已经是双向的**：`mm_s2_audit()` 走两遍树——status↔PTE、标志↔映射
  （`hyp.h:22-25`）。零拷贝页带的是同一面旗，所以审计逻辑一行都不用改。
- **契约已经把它排除在外**：`hyp_vcpu.h:83-86` 明确说共享零拷贝 VMO attach
  是后续切片，**不得在这里即兴发挥**。这是纪律，不是 TODO 注释。

### 3.3 阻塞项

- **COW 语义**。宿主 VMO 的页被借出去之后，宿主自己写它怎么办？今天的
  `mm_cow_from_status()`（`a455e3d3d`）是针对共享匿名页的；借出的页需要一条
  "借出期间不可写的页被写 → 先解除借用再 COW"的路径。这条路径的锁序要重新过
  `make check-mm-pt-lock-order`。
- **引用计数要跨 VM**。一个 VMO 页可以被两个 VM 同时借（一页两借）。今天帧的
  引用计数天然支持这件事，但 `FRAME_F_GUEST` 是布尔语义，不是"借给谁"的记录。
  审计要能回答"这一页借给了几个 VM"。
- **回收**。宿主任务退出时 VMO 销毁，被借出的页不能跟着回收。这是 VMO 销毁
  路径上的一个"外借引用"，形状与现有的 `charge_cg` 引用不同（`vmo.h:54`）。

---

## 4. guest virtio 设备模型（真 rootfs）

**状态：未落地。** 现在的设备模型是寄存器级的 16550 + CLINT，guest 没有块设备。
（但"所以进不了 shell"这个推论只在 `RAMFS_USER=0` 的镜像上成立，见 §4.1-4.2。）

**为什么排在 §3 之后**：virtio-blk 的数据面是 **DMA**。guest 的 virtqueue 在
guest RAM 里，设备要往它写 descriptor —— 这要求 stage-2 映射对设备可寻址，
而今天的 guest 帧只有"被 G-stage 走查"这一条路，没有"IOMMU / MMIO 写路径"。
零拷贝 VMO 先落地，guest 内存的身份与生命周期才有确定答案，DMA 才有落脚点。

**要做的事**：

| 项 | 说明 |
| --- | --- |
| virtio-mmio 传输 | guest 侧基址 `0x10001000` 起 8 个 slot（`platform.h:45`，`board.c` 里 `virtio_mmio_enumerate(VIRTIO_BASE, 8, 1)`）——模型要占住这个形状 |
| 配置空间 | RAZ/WI 太多会让 guest 的 virtio 驱动探测失败；关键是 `magic/device_id/version` 与特性位 |
| queue 通知 | `QueueNotify` 写入意味着 guest 认为数据已就绪。**要么**真做（读 guest 内存里的 descriptor，落到宿主 VMO 或临时页）**要么**明确返回失败并让 guest 看见 |
| DMA 隔离 | 设备写入必须被限制在 guest RAM 窗口内。这与 §3 的窗口语义是同一条边界的两种写法 |

### 4.1 块设备是不是"进 guest shell"的硬前提？分情况

**本节此前写的是"没有块设备，guest 挂不上 rootfs，会死在 `init_kthread`"，并把它
当成天花板的总解释。这句话对 `RAMFS_USER=0` 的 guest 成立，对 `RAMFS_USER=1` 的
guest 不成立。**

| guest 镜像 | 根 ramfs 里有没有 `/bin/init` | 需要块设备才能进 shell 吗 |
| --- | --- | --- |
| `RAMFS_USER=1` | 有。`init`、`mksh`、`help`、`ls`、`cat`、`ps`、`sleep`、`timer_preempt`、`timer_idle` 被 objcopy 成内核数据，`kernel/fs/rootfs_user.c:31-41` 的 overlay 由 `kernel/fs/diskfs/ramfs.c:1285-1301` 铺进根 ramfs（并软链 `/bin/sh -> /bin/mksh`）。程序清单在 `Makefile:1238` | **不需要**。`init_kthread()` 的默认 init 路径就是 `/bin/init`（`kernel/main.c:352`），会在根 ramfs 上命中 |
| `RAMFS_USER=0`（默认，`Makefile:111`） | 没有 | 需要。否则 `panic("init: no init program found")`（`kernel/main.c:361`） |

门禁 `smoke-hyp-a20os` 的构建变量是 `ARCH=riscv64 ABI=linux BRINGUP=0`
（`tools/smoke_cases.py`），即 `RAMFS_USER=0`，所以门禁里那条 "forbid 不含
PANIC" 的理由对门禁成立。
**本轮已构建并运行过 `RAMFS_USER=1` 的 guest**（`tools/targets-images.mk` 的
`$(GUEST_KERNEL_RAMFS_STAMP)` 规则构建它并以 `/boot/guest-kernel-ramfs.elf` 打进
FAT32；门禁 `smoke-hyp-console` boot 的就是它）。
**结果：它没有比 `RAMFS_USER=0` 走得更远**——同一个 `[RAMFS] Initialized`、同一个
`[SLAB BUG] kfree`，死在进 shell 之前。所以上表第二列"有 `/bin/init`"成立，
第三列"不需要块设备"成立，而**"能走到 shell"至今没有观测到**；拦住它的是 guest
自己 slab/RAMFS 交界处的那个 `kfree`，既不是 §4 的 virtio 也不是 §2 的中断注入
（[03-usage.md §3.3](03-usage.md)）。

**这条结论改变了本节的优先级**：如果目标只是"guest 能进 shell、跑 `ls`/`cat`/`ps`"，
那么本节可以整体推迟，改去做 §4.2 的 ramfs-user guest 那条路；如果目标是"guest 读
镜像上的文件、能持久写"，本节仍然是硬前提。**先说清哪个是要的，再决定顺序**——
本文件此前默认了后者而没有说明。

### 4.2 三条降低 virtio 必要性的事实

1. **guest 的 virtio-blk 是内核内驱动，不是用户态程序。**
   `kernel/drvmod/examples/virtio_blk.c` 是 16 行注册壳，`#include` 了
   `kernel/drivers/block/virtio_blk.c`（1154 行实现），声明为
   `A20_DRIVER_PLACEMENT_KERNEL_MODULE`（`components/drivers.toml:20-24` 登记成
   early `.a20drv`，`kernel/arch/riscv64/platform/early_drivers.c:33` 从
   `/boot/drivers` 加载）。所以这一项**不需要 guest 侧新增任何用户态代码**，
   需要的是设备侧把那套驱动能驱动起来。
2. **中断注入不是块设备能工作的硬前提。** `virtio_blk_wait_req()` 在
   `!inst->irq_registered` 时直接抽 used ring、`proc_wake_q_flush()` 唤醒并
   `continue`（`kernel/drivers/block/virtio_blk.c:749-763`，注释明写
   "Poll-only transports cannot wake a blocked task through an IRQ"）。也就是说
   一个纯轮询的 virtio-mmio 设备模型就够 guest 把块设备用起来，§2 的外部中断注入
   可以排在它后面。
3. **`RAMFS_USER=1` 的 guest 根本不需要块设备**（§4.1 第一行）。

综合 1–3：**§2 的外部中断注入与本节的 virtio 设备模型之间没有依赖关系**，两者都
排在"guest 活过早期启动"之后，而不是互相阻塞。

### 4.3 替代路径

让 guest 通过已有的驱动栈看到宿主的块设备，而不是 virtio。这条路只在 stage-2 上
加一段地址翻译（guest 物理地址 → 宿主 VMO），不做任何队列仿真。代价是 guest 不是
"真机器"，但它能挂真 rootfs。如果目标只是"guest 跑真文件系统"，这条路的性价比高
一个数量级。

---

## 5. x4 根表（512 GiB 限制）

**状态：未落地，且这是本文件里唯一的 `pt_meta_t` 格式变更。**

当前 hgatp 是 Sv39x4（`hgatp` 的 x4 模式），但 `hyp.c` 只编程根表 [0, 512) 的
条目，根表保持 512 条目的常规 `pt_meta_t`（`kernel/include/hyp/hyp.h:26-30`）。
后果是 guest 物理内存必须低于 512 GiB。

解除它需要：2048 条目的 x4 根表（16 KiB）+ 更宽的根元数据块 + 非叶 PTE 的
**分段 PPN 编码**（x4 扩展里非叶 PPN 是拆成两段的，当前这台 hart 用的是一个
连续字段，见 `hyp_arch.c:44-55` 的说明）。这三条都不是本切片能顺手带过的：

- `pt_meta_t` 是格式，改它就是改所有页表页的布局；
- 根表节点锁与审计的并发故事要重新过一遍；
- 非叶 PPN 编码是**这颗 hart 的实现选择**，不是规格强制的，所以"支持 x4"这件事
  本身要先确认目标平台。

按 `00-design.md` §2 的纪律（格式变更刻意缓行），这一项排在功能性的切片之后。

---

## 6. 真机验证

**状态：未落地。** 到目前为止 hypervisor 的全部行为只在 QEMU TCG 上验证过
（QEMU 10.0.13，Debian 1:10.0.13+ds-0+deb13u1，`-cpu rv64,h=true`）。

TCG 与真实 H 扩展 CPU 已知不同的几处，都是本设计依赖的：

| 项 | TCG 现状 | 真机要重验的 |
| --- | --- | --- |
| `hfence.gvma` | 走查是软件模拟 | TLB 失效语义、VMID 宽度 |
| `hgatp` 的 MODE 编号 | 8 = Sv39 三级 | 实现可能给别的编号 |
| `hcounteren` 的 WARL 位 | — | QEMU 常把位实现成全 1，真机可能不是 |
| VS CSR 影子 | — | 影子是否真的不陷到 HS |
| `hideleg` 位翻译（2/6/10 → 1/5/9） | — | 规格要求，实现未必全 |
| 嵌套虚拟化 | 不支持 | `hstatus.HU` 与 HLV/HLVX 指令的真值 |

**现有真机**：`kernel/platform/` 下有 VisionFive 2（JH7110）与 LS2K1000
（LoongArch）两家带 H 能力的 riscv64/其它平台板子，其中 licheerv-nano 与
milk-v-duo 在树里是完整板级实现（[platforms/porting-guide.md](../platforms/porting-guide.md)）。
**具体哪一块硬件的 H 扩展支持哪几条（尤其 x4 与 `hcounteren` 的 WARL 位），
本片没有查证**，是上真机前的第一件事。

**上真机时的门禁增量**：现有的 `smoke-hyp-selftest` 与 `smoke-hyp-vcpu`
都硬编码了 `-cpu rv64,h=true`，真机要用等价的启动参数重跑同一批断言；
`00-design.md` §6 的表里那几条（`smoke-mm-stress`、锁模型门禁）与硬件无关，
可直接复用。

---

## 7. 不在计划里的事

明确写下，免得以后被当成漏项：

- **guest 之间的隔离 / vmid 复用**。今天一次只跑一个 guest，隔离没有对象。
  `00-design.md` §2 的"VMID 只增不重用"是隔离需求出现之前的权宜之计，不是终态。
- **vCPU 的热插拔 / 保存恢复**。能力叙事的另一条线（`docs/research/`），与
  本文件正交。
- **非 riscv64 架构**。arm32/loongarch 走 `hyp_supported()==0` 的 stub
  （`00-design.md` §1），没有 stage-2 硬件就不做。
- **guest 的嵌套虚拟化**。`hstatus.HU=0`（`hyp_vcpu.h` v2 段的 setup 说明），
  guest 不做虚拟化指令。