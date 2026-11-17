# hypervisor 路线图

> 说明：多架构后端与多 vCPU 并发规范见 [后端说明](08-multiarch-backends.md) 与 [运行验证](09-smp-validation.md)；现行操作命令与实现边界以 [使用指南](03-usage.md) 和 [实现状态](../status/current/hypervisor-implementation-status.md) 为准。

本文梳理 Hypervisor 子系统的演进切片顺序与前置条件。已落地的地基设计见 [00-design.md](00-design.md)，Guest 协议规范见 [01-a20os-guest.md](01-a20os-guest.md)。

核心子项的实际实现与验证状态：

- **§2.2 guest 时钟中断路径**：基于硬件行为直接生效，无需 HS 注入（依据见 §2.2）。当前时钟初始化已就绪，时钟抢占与 tick 深度验证待后续长跑观测。
- **§4.1 rootfs 与启动环境**：`RAMFS_USER=0` 镜像无 `/bin/init`，会触发预期的 panic 关机；`RAMFS_USER=1` 镜像则内嵌完整的 `/bin/init` 与用户态工具，无需块设备即可完成独立引导。
- **guest 控制台输入（契约 v3）**：通过轮询 `LSR`/`RBR` 寄存器实现控制台双向交互（参见 [03-usage.md §3.4](03-usage.md)），不依赖中断注入，由门禁 `smoke-hyp-console-p0` 覆盖验证。
- **完整用户态交互（`smoke-hyp-shell`）**：在真 `RAMFS_USER=1` guest 下，系统可顺利进入 mksh 提示符，执行测试交互命令并正常退出。关键修复包括合成 FDT 的 4 字节对齐补齐（解决解析器脱同步与内存越界问题）以及 guest 入口帧浮点权限初始化（`mstatus.FS=Initial`，防止非法指令陷入导致栈损坏）。
- **交互关机与退出时序**：当前内嵌根文件系统（`RAMFS_USER_PROGRAMS`，`Makefile:1238-1241`）未包含独立的 `poweroff` 实用程序。结束交互通过在 mksh 中执行 `exit`，由 `user/init.c` 的 `waitpid` 循环捕获后调用 `reboot(RB_POWER_OFF)`，宿主感知为 `HYP_EXIT_SHUTDOWN` 完成优雅关机。
- **控制台输入排队与关机时序**：使用批量输入时需注意输入环缓冲的消费窗口，避免过早输入的关机指令被 guest 内部丢弃。现行控制台门禁将收尾步骤挂载于 `PASS` 标记之后，保证断言与退出时序确定。

顺序不是偏好，但也不是一条链。修正后的依赖关系：

- §1 的 CSR 地基与 §2 的 CPU-local 多 vCPU/SBI 路径已有实现；完整 OS guest SMP 与外部设备中断仍按 §2 验收。
- §3（零拷贝 VMO guest RAM）与 §4（virtio 设备模型）之间的依赖比原先写的弱：§4.2 第 2 条给出 virtio-blk 的纯轮询完成路径（`kernel/drivers/block/ virtio_blk.c:749-763`），所以 §4 不必等 §2 的中断注入；但 §4 的 DMA 确实要§3 之后才有着落点（guest 内存的身份与生命周期要有确定答案）。
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

     §2 的中断注入 ──×─→ §4：两者无依赖（virtio-blk 有 poll-only 完成路径）
     §4.1 的 RAMFS_USER=1 guest ──×→ §4：那条 guest 根本不需要块设备
```

---

## 0.1 从 KVM 借鉴了什么，以及明确不借鉴什么

[01-a20os-guest.md §9](01-a20os-guest.md) 梳理了与 KVM 的架构差异。本节进一步说明架构设计中参考 KVM 的机制以及明确做出剪裁的设计权衡：

### 借鉴的（有代码或契约对应）

| KVM 的做法 | 本树的对应物 | 依据 |
| --- | --- | --- |
| guest trap 借宿主 trap 路径，`sscratch` + per-vcpu trampoline bank | 同样借标准 trap 路径，`vcpu->tramp[]` 是那段 bank，汇编可见偏移钉在 `HYP_VCPU_TRAMP_U64` | `kernel/include/hyp/hyp_vcpu.h:46-52`、`kernel/arch/riscv64/hyp/hyp_asm_offsets.h` |
| 宿主页表机制（Guest memory slots、缺页回调、map/unmap 按页） | stage-2 页表 + `hyp_ram_fill()` 按页补零 + `mm_s2_audit()` 双向审计 | `kernel/hyp/hyp.c`；`kernel/hyp/hyp_vcpu.c:186-259` |
| vcpu run loop 是一个"进 guest / 陷出来 / 分类 / sret 回去"的循环 | 同款四段，分类器是 `hyp_arch_guest_trap()` 与 `hyp_vcpu_handle_trap()` | `kernel/arch/riscv64/hyp/hyp_emulate.c:730`、`hyp_vcpu_asm.S` |
| "trap 帧就是 guest 状态，不必另存一份" | 同样：`hyp_vcpu_handle_trap()` 只改 `vcpu->regs[]`/`pc`，从不碰交给它的帧 | `kernel/arch/riscv64/hyp/hyp_emulate.c:730`（`(void)trap_frame;`） |
| guest 的 vCPU 创建要有 canary / magic 防止野指针 | `HYP_VM_MAGIC` / `HYP_VCPU_MAGIC`，每次入口都查 | `kernel/arch/riscv64/hyp/hyp_emulate.c:730` |

**一条借鉴的形状值得单独说**：最初的设计提过"KVM-riscv 的 sscratch 双区方案"，落地时**没有采用**——guest trap 走的是内核自己的标准 trap 路径，帧本身就是 guest 状态（`00-design.md` §5 的"帧即状态 / 唯一接缝"）。所以这里借鉴的是 KVM 的**接口形状**，不是它的**实现位置**：KVM 把 guest 的 trap 状态放在自己的数据结构里，本树让 guest 状态住在内核的 trap 帧里。代价是宿主 IRQ 必须在 guest 帧上就地跑完再 `sret`（`kernel/arch/riscv64/hyp/hyp_emulate.c:730` 的即时返回），收益是不需要第二套寄存器保存代码。

### 明确**不**借鉴的，以及为什么

| 不借鉴 | 理由 |
| --- | --- |
| **用户态设备模型**（KVM 把 virtio / PCI 整个放在 QEMU 里） | 本树的 guest 帧要过 `mm_s2_audit()` 的双向交叉检查（status↔PTE、标志↔映射）。设备要往 guest 内存写 DMA，目标地址必须能被 stage-2 翻译并且能被审计；这条约束在用户态就没有对应物（`01-a20os-guest.md` §9 的"核心差别只有一条"） |
| **memslot / `KVM_SET_USER_MEMORY_REGION` 式的地址空间登记** | 同上。guest RAM 是一组被出借的帧（`FRAME_F_GUEST` 身份标志 + 引用计数），不是一个由外部登记的地址区间。零拷贝（§3）是把同一个语义推到 VMO 级，不是把 memslot 搬进来 |
| **注入式时器中断**（KVM 在 vcpu 定时器到点时置 `hvip.VSTIP`） | 在 H 扩展 + Sstc 上这是多余的一次写：vstimecmp 到期由硬件置 `mip.VSTIP`，`hideleg` bit 6 已委托（见 [RISC-V SBI](10-riscv-sbi.md)）。这正是"借鉴"与"照抄"分界的地方——KVM 需要注入是因为它没有 sstc 的 guest 面 |
| **用户态 /dev/kvm ioctl 接口 + QEMU 的 vCPU 抽象** | 本树的机器形状收进内核（`01-a20os-guest.md` §9）。用户入口是 Native ABI 的几个调用加上 Linux ABI 桥 |
| **guest 内存的页保护分级**（KVM 允许按 slot 标 RWX） | 本设计一次只跑一个 guest，窗口页一律 RWX（`01-a20os-guest.md` §10 第 3 条）。等 guest 之间的隔离真的成为对象时再分级 |

**一句话**：我们借鉴 KVM 的是**接口形状与那几条已经被反复验证过的循环结构**；不借鉴的是**"机器的形状放在用户态"这个前提**，以及它带来的两个后果（用户态设备模型、memslot 记账）。这两条在 `01-a20os-guest.md` §9 里已经写成表格，本节补的是**为什么不做**——因为 stage-2 页表在单级内存模型里是一等公民，拆到用户态就失去审计。

## 1. HS-mode CSR 状态

RISC-V vCPU 已保存/恢复 VS CSR、FP、计时器与 IRQ 状态；相关实现及约束见 [实现状态](../status/current/hypervisor-implementation-status.md) 与 [RISC-V SBI](10-riscv-sbi.md)。

## 2. SMP 与中断虚拟化的剩余范围

CPU-local vCPU 激活、双宿主 CPU 上双 vCPU 并行以及 SBI HSM/IPI/RFENCE 已实现；基础运行与负控见 [SMP 验证记录](09-smp-validation.md)，RV32/RV64 SBI 专项见 [RISC-V SBI](10-riscv-sbi.md)。

仍需完成：

- 既有 RISC-V H 的完整 OS SMP、双方暂停/恢复与同宿主 100 次生命周期已通过，见 [验收状态](../status/current/hypervisor-implementation-status.md#2026-11-16-生命周期收口)。
- guest PLIC/外部设备中断模型与注入；已实现的软件 IPI 和时钟路径不替代外部设备中断。
- HSM suspend 与 guest reboot，见 SBI 的接口边界。

轮询 UART 控制台不依赖外部中断注入；后续设备模型按各自的数据面、DMA 与中断要求独立验收。

---

## 3. 零拷贝 VMO guest RAM —— capability 叙事的正主

**状态：未落地。** 当前的 guest 帧由 `hyp_ram_fill()` 从帧分配器现取（[01 §5](01-a20os-guest.md)），是**复制/新建**出来的，不共享任何宿主内存。

### 3.1 为什么这是正主

capability 叙事的核心主张是"资源以能力的形式出借，宿主与客户共享同一个对象，而不是各持一份"。在内存这条线上，它应该长这样：

```
今天：  guest 页  =  hyp_ram_fill() 从 pfa 现分配的一帧      （guest 独占）
零拷贝：  guest 页  =  宿主 VMO 的某一页，出借进 stage-2      （宿主 VMO 与 guest 共用）
```

差别不是性能，是**身份**。今天 guest 用过的内存在 `pfa` 的统计里是"guest 的内存"；零拷贝之后同一页在 VMO 的记账里是"某个进程的内存，只是被借给了某个 VM"。 cgroup 记账也才真正有意义——借出的页记在**持有 VMO 的那个任务的 cgroup** 上，而 VM 自己没有预算身份，这与 §3.2 的记账改动是同一件事。

### 3.2 树里已经有的东西

- **出借机制已经是帧级的**：`FRAME_F_GUEST` 身份标志 + 引用计数，`mm_pt_frame_lend/return` 走同一条路（`kernel/include/hyp/hyp.h:22-24`）。零拷贝不是新机制，是把 `frame_alloc` 换成 `vmo_get_page()`。
- **记账接口是现成的**：`vmo_get_page_charged()`（`kernel/mm/vmo.c:152,242`）就是"按页物化并记到 cgroup"，`cg_mem_charge()` / `cg_mem_uncharge()`（`vmo.c:276,284`）作为核心资源计费接口。
- **审计已经是双向的**：`mm_s2_audit()` 走两遍树——status↔PTE、标志↔映射（`hyp.h:22-25`）。零拷贝页带的是同一面旗，所以审计逻辑一行都不用改。
- **契约已经把它排除在外**：`hyp_vcpu.h:83-86` 明确说共享零拷贝 VMO attach 是后续切片，**不得在这里即兴发挥**。这是纪律，不是 TODO 注释。

### 3.3 阻塞项

- **COW 语义**。宿主 VMO 的页被借出去之后，宿主自己写它怎么办？今天的`mm_cow_from_status()`（`a455e3d3d`）是针对共享匿名页的；借出的页需要一条"借出期间不可写的页被写 → 先解除借用再 COW"的路径。这条路径的锁序要重新过`make check-mm-pt-lock-order`。
- **引用计数要跨 VM**。一个 VMO 页可以被两个 VM 同时借（一页两借）。今天帧的引用计数天然支持这件事，但 `FRAME_F_GUEST` 是布尔语义，不是"借给谁"的记录。审计要能回答"这一页借给了几个 VM"。
- **回收**。宿主任务退出时 VMO 销毁，被借出的页不能跟着回收。这是 VMO 销毁路径上的一个"外借引用"，形状与现有的 `charge_cg` 引用不同（`vmo.h:54`）。

---

## 4. guest virtio 设备模型（真 rootfs）

**状态：未落地。** 现在的设备模型是寄存器级的 16550 + CLINT，guest 没有块设备。（但"所以进不了 shell"这个推论只在 `RAMFS_USER=0` 的镜像上成立，见 §4.1-4.2。）

**为什么排在 §3 之后**：virtio-blk 的数据面是 **DMA**。guest 的 virtqueue 在 guest RAM 里，设备要往它写 descriptor —— 这要求 stage-2 映射对设备可寻址，而今天的 guest 帧只有"被 G-stage 走查"这一条路，没有"IOMMU / MMIO 写路径"。零拷贝 VMO 先落地，guest 内存的身份与生命周期才有确定答案，DMA 才有落脚点。

**要做的事**：

| 项 | 说明 |
| --- | --- |
| virtio-mmio 传输 | guest 侧基址 `0x10001000` 起 8 个 slot（`platform.h:45`，`board.c` 里 `virtio_mmio_enumerate(VIRTIO_BASE, 8, 1)`）——模型要占住这个形状 |
| 配置空间 | RAZ/WI 太多会让 guest 的 virtio 驱动探测失败；关键是 `magic/device_id/version` 与特性位 |
| queue 通知 | `QueueNotify` 写入意味着 guest 认为数据已就绪。**要么**真做（读 guest 内存里的 descriptor，落到宿主 VMO 或临时页）**要么**明确返回失败并让 guest 看见 |
| DMA 隔离 | 设备写入必须被限制在 guest RAM 窗口内。这与 §3 的窗口语义是同一条边界的两种写法 |

### 4.1 块设备是不是"进 guest shell"的硬前提？分情况

**本节此前写的是"没有块设备，guest 挂不上 rootfs，会死在 `init_kthread`"，并把它当成天花板的总解释。这句话对 `RAMFS_USER=0` 的 guest 成立，对 `RAMFS_USER=1` 的 guest 不成立。**

| guest 镜像 | 根 ramfs 里有没有 `/bin/init` | 需要块设备才能进 shell 吗 |
| --- | --- | --- |
| `RAMFS_USER=1` | 有。`init`、`mksh`、`help`、`ls`、`cat`、`ps`、`sleep`、`timer_preempt`、`timer_idle` 被 objcopy 成内核数据，`kernel/fs/rootfs_user.c:31-41` 的 overlay 由 `kernel/fs/diskfs/ramfs.c:1285-1301` 铺进根 ramfs（并软链 `/bin/sh -> /bin/mksh`）。程序清单在 `Makefile:1238` | **不需要**。`init_kthread()` 的默认 init 路径就是 `/bin/init`（`kernel/main.c:352`），会在根 ramfs 上命中 |
| `RAMFS_USER=0`（默认，`Makefile:111`） | 没有 | 需要。否则 `panic("init: no init program found")`（`kernel/main.c:361`） |

门禁 `smoke-hyp-a20os` 的构建变量是 `ARCH=riscv64 ABI=linux BRINGUP=0`（`tools/smoke_cases.py`），即 `RAMFS_USER=0`，所以门禁里那条 "forbid 不含 PANIC" 的理由对门禁成立。在采用 `RAMFS_USER=1` 构建的 guest 中（通过 `/boot/guest-kernel-ramfs.elf` 验证），系统成功进入 shell 并通过 `make smoke-hyp-shell` 门禁验证（[03-usage.md §3.4.2](03-usage.md)）。所以上表第二列"有 `/bin/init`"与第三列"不需要块设备"成立。此前排查中解决的关键问题包括合成 FDT 的 4 字节补齐与 guest 入口帧浮点寄存器权限初始设置（[03-usage.md §3.3 第五次](03-usage.md)）。

**这条结论改变了本节的优先级**：如果目标只是"guest 能进 shell、跑 `ls`/`cat`/`ps`"，那么本节可以整体推迟，改去做 §4.2 的 ramfs-user guest 那条路；如果目标是"guest 读镜像上的文件、能持久写"，本节仍然是硬前提。**先说清哪个是要的，再决定顺序**——本文件此前默认了后者而没有说明。

### 4.2 三条降低 virtio 必要性的事实

1. **guest 的 virtio-blk 是内核内驱动，不是用户态程序。** `kernel/drvmod/examples/virtio_blk.c` 是 16 行注册壳，`#include` 了`kernel/drivers/block/virtio_blk.c`（1154 行实现），声明为`A20_DRIVER_PLACEMENT_KERNEL_MODULE`（`components/drivers.toml:20-24` 登记成 early `.a20drv`，`kernel/arch/riscv64/platform/early_drivers.c:33` 从`/boot/drivers` 加载）。所以这一项**不需要 guest 侧新增任何用户态代码**，需要的是设备侧把那套驱动能驱动起来。
2. **中断注入不是块设备能工作的硬前提。** `virtio_blk_wait_req()` 在`!inst->irq_registered` 时直接抽 used ring、`proc_wake_q_flush()` 唤醒并`continue`（`kernel/drivers/block/virtio_blk.c:749-763`，注释明写"Poll-only transports cannot wake a blocked task through an IRQ"）。也就是说一个纯轮询的 virtio-mmio 设备模型就够 guest 把块设备用起来，§2 的外部中断注入可以排在它后面。
3. **`RAMFS_USER=1` 的 guest 根本不需要块设备**（§4.1 第一行）。

综合 1–3：**§2 的外部中断注入与本节的 virtio 设备模型之间没有依赖关系**，两者都排在"guest 活过早期启动"之后，而不是互相阻塞。

### 4.3 替代路径

让 guest 通过已有的驱动栈看到宿主的块设备，而不是 virtio。这条路只在 stage-2 上加一段地址翻译（guest 物理地址 → 宿主 VMO），不做任何队列仿真。代价是 guest 不是"真机器"，但它能挂真 rootfs。如果目标只是"guest 跑真文件系统"，这条路的性价比高一个数量级。

---

## 5. x4 根表（512 GiB 限制）

**状态：未落地，且这是本文件里唯一的 `pt_meta_t` 格式变更。**

当前 hgatp 是 Sv39x4（`hgatp` 的 x4 模式），但 `hyp.c` 只编程根表 [0, 512) 的条目，根表保持 512 条目的常规 `pt_meta_t`（`kernel/include/hyp/hyp.h:26-30`）。后果是 guest 物理内存必须低于 512 GiB。

解除它需要：2048 条目的 x4 根表（16 KiB）+ 更宽的根元数据块 + 非叶 PTE 的**分段 PPN 编码**（x4 扩展里非叶 PPN 是拆成两段的，当前这台 hart 用的是一个连续字段，见 `hyp_arch.c:44-55` 的说明）。这三条都不是本切片能顺手带过的：

- `pt_meta_t` 是格式，改它就是改所有页表页的布局；
- 根表节点锁与审计的并发故事要重新过一遍；
- 非叶 PPN 编码是**这颗 hart 的实现选择**，不是规格强制的，所以"支持 x4"这件事本身要先确认目标平台。

按 `00-design.md` §2 的纪律（格式变更刻意缓行），这一项排在功能性的切片之后。

---

## 6. 真机验证

**状态：RISC-V/ARM 物理板与 Intel 实机尚未验收。** 已有 QEMU TCG、AMD 嵌套 KVM 和 Bochs 软件 CPU 的后端运行记录，范围见 [多架构验证](09-smp-validation.md)；它们不能替代物理板验收。

TCG 与真实 H 扩展 CPU 已知不同的几处，都是本设计依赖的：

| 项 | TCG 现状 | 真机要重验的 |
| --- | --- | --- |
| `hfence.gvma` | 走查是软件模拟 | TLB 失效语义、VMID 宽度 |
| `hgatp` 的 MODE 编号 | 8 = Sv39 三级 | 实现可能给别的编号 |
| `hcounteren` 的 WARL 位 | — | QEMU 常把位实现成全 1，真机可能不是 |
| VS CSR 影子 | — | 影子是否真的不陷到 HS |
| `hideleg` 位翻译（2/6/10 → 1/5/9） | — | 规格要求，实现未必全 |
| 嵌套虚拟化 | 不支持 | `hstatus.HU` 与 HLV/HLVX 指令的真值 |

**现有真机**：`kernel/arch/<arch>/boards/` 下有 VisionFive 2（JH7110）与 LS2K1000 （LoongArch）两家带 H 能力的 riscv64/其它平台板子，其中 licheerv-nano 与 milk-v-duo 在树里是完整板级实现（[platforms/porting-guide.md](../platforms/porting-guide.md)）。**具体哪一块硬件的 H 扩展支持哪几条（尤其 x4 与 `hcounteren` 的 WARL 位），本片没有查证**，是上真机前的第一件事。

**上真机时的门禁增量**：现有的 `smoke-hyp-selftest` 与 `smoke-hyp-vcpu`都硬编码了 `-cpu rv64,h=true`，真机要用等价的启动参数重跑同一批断言；`00-design.md` §6 的表里那几条（`smoke-mm-stress`、锁模型门禁）与硬件无关，可直接复用。

---

## 7. 不在计划里的事

明确写下，免得以后被当成漏项：

- **guest 之间的隔离 / vmid 复用**。今天一次只跑一个 guest，隔离没有对象。`00-design.md` §2 的"VMID 只增不重用"是隔离需求出现之前的权宜之计，不是终态。
- **vCPU 的热插拔 / 保存恢复**。能力叙事的另一条线（`docs/zh/research/`），与本文件正交。
- **非 riscv64 架构**。arm32/loongarch 走 `hyp_supported()==0` 的 stub （`00-design.md` §1），没有 stage-2 硬件就不做。
- **guest 的嵌套虚拟化**。`hstatus.HU=0`（`hyp_vcpu.h` v2 段的 setup 说明）， guest 不做虚拟化指令。