# hypervisor 路线图

最后核实：本文记录的是 v2（`01-a20os-guest.md`）之后的切片顺序与各自的
前置条件。**§0–§6 没有任何一项已落地**，写在这里是为了说明"为什么是这个顺序"以及每一
项被什么卡着。已落地的部分见
[00-design.md](00-design.md)，v2 的设计见 [01-a20os-guest.md](01-a20os-guest.md)。

顺序不是偏好。§2 的前三项是**不加就看不到下一项**的：没有中断，guest 跑不完
调度；没有 tick，就没有真实负载；没有块设备，就没有 rootfs。§4 起开始触及
内存模型本身的格式变更，按 P7 的纪律（`00-design.md` §2）刻意缓行。

---

## 0. 依赖关系一览

```
v2 已定义的委托/设备/引导面
        │
        ├─ §1 hstatus/hcounteren 规格化 ─┐
        ├─ §2 多 vcpu + 中断虚拟化 ──────┤（需要 §1 的 CSR 规格先定死）
        │                                │
        │        ┌───────────────────────┘
        │        ↓
        │   §3 零拷贝 VMO guest RAM（capability 叙事的正主）
        │        │
        │        └─→ §4 guest virtio 设备模型（真 rootfs）
        │
        └─ §5 x4 根表 / §6 真机验证：与上面正交，可在任意时点插入
```

---

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
| `hstatus.VGEIN` / `hgeip` / `hgeie` | §2 的 VSEIP 转发 | 读 CSR 得到未知值或全零 |
| `hvip.VSTIP` / `VSSIP` | §2 制造 guest 时钟中断 | guest 永远等不到 tick |
| `hgatp` 的 MODE/VMID/PPN 布局 | G-stage 走查 | 见 `00-design.md` §3 的 `hyp_make_hgatp()` |

**一句需要纠正的话**：`hstatus` 里**没有**中断使能字段。QEMU 的
`target/riscv/cpu_bits.h:612-623` 只定义了 VSBE/GVA/SPV/SPVP/HU/VGEIN/VTVM/VTW/
VTSR/HUKTE/VSXL/HUPMM——**bit 5 是 VSBE，bit 10 没有对应字段**。所以
**这台平台上"HS 侧的 guest 中断使能"不是 hstatus 的字段**，那一类问题不能靠改
hstatus 解决，必须走 §2 的"HS 处理完自己的中断后用 `hvip` 断言 guest 中断"这条
路。把这件事写进规格文档，是为了让下一个读代码的人不必重新推导一遍。

不过要说清楚当前代码的实际状态，免得读者按本文去找一句并不存在的注释：工作树
**确实把 bit 10 编进了 `HYP_HSTATUS_GUEST_ON`**——`kernel/arch/riscv64/hyp/
hyp_arch.c:116-117`（C）与 `kernel/arch/riscv64/hyp/hyp_vcpu_asm.S:52-53`（.S）
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
（`kernel/hyp/hyp_vcpu.c:74-88,311-322`）。

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

guest 要有 tick，HS 就得能制造 guest 中断。规格给了三条路：

1. **`hvip.VSTIP` / `VSSIP` / `VSEIP` 是可写的**——HS 直接置位即向 VS 断言
   虚拟中断。`VSSIP` 在 `hip` 里是可写别名，可以清掉；`VSTIP`/`VSEIP` 的 pending
   是 `hvip` 位与（sstc 情形下）`vstimecmp` 的逻辑或，清除方式由实现提供。
2. **`hgeip` / `hgeie` + `hstatus.VGEIN`**——把 guest 外部中断号映射到 VS 级
   外部中断。这是 PLIC 的正路。
3. **让 guest 的 `vstimecmp` 真的驱动物理 CLINT**——需要 H 扩展实现 sstc 的
   guest 面；QEMU 是否实现，**本片未验证**。

v2 的 SBI 面（[01 §4](01-a20os-guest.md)）里没有 `set_timer`，guest 走的是
sstc 那条路。这条路能否走通直接决定 §2 的形态，所以 §1 的规格化和 §2 的第一件
事是同一个问题。

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

**状态：未落地。** 现在的设备模型是寄存器级的 16550 + CLINT，
guest 只能有 initramfs。

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

**当前的天花板就在这里**：没有块设备，guest 挂不上 rootfs，会死在
`init_kthread`（`kernel/main.c` 的 "init: no init program found"）。所以
`smoke-hyp-a20os` 的 PASS 定义成"guest 到达了自己的 banner"而不是"guest 引导
完成"（[01 §8.3](01-a20os-guest.md)）。这一项做完，天花板才从"到达 banner"
抬到"挂上 rootfs 跑 init"。

**替代路径（成本低得多，值得先比一比）**：让 guest 通过已有的驱动栈看到宿主
的块设备，而不是 virtio。这条路只在 stage-2 上加一段地址翻译（guest 物理地址
→ 宿主 VMO），不做任何队列仿真。代价是 guest 不是"真机器"，但它能挂真 rootfs。
如果目标只是"guest 跑真文件系统"，这条路的性价比高一个量级。

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