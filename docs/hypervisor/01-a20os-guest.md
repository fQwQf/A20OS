# v2：A20OS 作为 guest

最后核实：契约已落地（`6364d1138`，`kernel/include/hyp/hyp_vcpu.h` 的 v2 段）。
**实现半在工作树里**：设备模型（`kernel/hyp/hyp_dev.c`）、RAM 窗口与 marker
（`kernel/hyp/hyp.c`、`kernel/include/hyp/hyp.h`）、Linux ABI 桥新增的三个调用、
装载器 `user/cmds/core/hyp_boot.c` 与门禁 `smoke-hyp-a20os`
（`tools/smoke_cases.py`），以及**架构半**（`kernel/arch/riscv64/hyp/`）——但
**全部未提交**，HEAD 上只有契约。

§2.3 的两条修正里，**（一）`HYP_HIDELEG_DEFAULT` 已修**：掩码改为 VS 级位
2/6/10（`kernel/include/hyp/hyp_vcpu.h:216-222`）。**（二）`hstatus.VTVM` 未按本文
原建议去掉**，代码选择保留 VTVM=1 并在 scause 22 上模拟 guest 的 `csrw satp` /
`sfence.vma` / `csrw vstimecmp`（`kernel/hyp/hyp_vcpu.c:927-1015`）；本文 §2.3(二)
与 §3 的相关段落已按**已实现的策略**改写，理由见那里。v1 的已落地范围见
[00-design.md](00-design.md)，后续切片见 [02-roadmap.md](02-roadmap.md)。

标注约定：**已落地** = HEAD 有对应代码；**进行中** = 工作树里有未提交的
v2 实现，本片读过但没有运行过；**未验证** = 设计推演或依赖外部行为，本片没有
跑过任何东西。

---

## 1. v1 与 v2 的差别

| | v1（已落地） | v2（进行中） |
| --- | --- | --- |
| guest | 56 字节手写指令序列 | 完整的 A20OS 内核 |
| 委托 | `hedeleg`/`hideleg` 全 0，一切陷到 HS | 异常几乎全委托给 VS，中断按 VS 级掩码委托 |
| guest 内存 | 全部由 `hyp_vm_load()` 预先装载 | 预装镜像 + 按需 RAM 窗口（guest 自己分配的页表/bss/栈） |
| 设备 | 无（guest 不碰 MMIO） | 16550 UART + CLINT mtime 的最小设备模型 |
| guest 时钟 | 无 | `time` CSR 硬件虚拟（htimedelta）+ CLINT mtime 读 |
| 引导 | `vcpu->pc = entry_gpa`，其余全零 | `a0=hartid`、`a1=DTB GPA`、最小 FDT |
| 判据 | console 上一行 `^HYP$` | guest console marker 命中（不看宿主日志交错） |

一句话概括 v2 的思路：**把 hypervisor 从"模拟一台机器"改成"替一台机器挡住外面"。
guest 自己管自己的中断与缺页，HS 只在两个地方动手——二级缺页补页，和设备
MMIO**。这条思路直接决定了委托表（§2）与路由表（§5）。

---

## 2. 委托表

### 2.1 掩码与理由

| CSR | 值 | 理由 |
| --- | --- | --- |
| `hedeleg` | 全 1 **减去 ecall-from-VS 那一项**（cause 10） | guest 自己管自己的 trap，包括给用户进程的缺页。只有 SBI 面必须由 HS 承接的那一个 ecall 留给 HS。掩码按"全 1 减一位"写，见下方关于逐位可写性的说明。 |
| `hideleg` | **bit 2 \| bit 6 \| bit 10**（VSSIP \| VSTIP \| VSEIP） | 这三位是 `hideleg` 里的**可写** VS 级中断位。委托之后硬件会把它们翻译成 VS-mode 看到的 supervisor 软件/时钟/外部中断（cause 1/5/9），guest 的 `sie` 与 `vs tvec` 就能像在裸机上一样派发，`WFI` 也在被委托中断到来时自然醒来，不需要 HS 模拟 WFI。 |
| `hstatus.SPV` | 1 | trap 返回的目标是虚拟态，这是 HS 侧唯一的"我在跑 guest"标志 |
| `hstatus.SPVP` | 1 | 允许 HS 读写 guest 的 `vs*` CSR（引导时读回 vsatp、诊断时用） |
| `hstatus.VTVM` | **1** | 见 §3 与 §2.3(二)：保留 VTVM，改在 scause 22 上模拟被拦下的 guest CSR 写 |
| `hstatus.HU` | 0 | guest 不做虚拟化指令 |
| `hcounteren.TM` | **1** | 见 §3 |
| `htimedelta` | 0 | guest 与宿主看到同一个单调时钟，见 §3 |

### 2.2 关于 `hedeleg` 的逐位可写性

规格 `hypervisor.adoc` 的 "Bits of `hedeleg` that must be writable or must be
read-only zero" 一表，在本版本源码里与同一文件的编号 cause 表对不齐：位列表跳过
14 却一直写到 23，而异常列只有 0..21 共 22 项。因此本文**不逐位列举 `hedeleg`
的可写位**，只写"全 1 减一位"——只读零的位写了也无效，两种情况下行为一致。

真正需要确认的实质问题只有一个：**ecall-from-VS 必须没有被委托**。委托了它就
只到 VS 自己，HS 的 SBI 面永远不会被调用；而它是 guest 唯一能向 HS 说话的口子。
这个要求与该位是否可写无关，写掩码时按"没被委托"来验证即可。

### 2.3 契约里写错的两处（**必须先修**）

这两条不是设计选择，是照抄会得到错误行为的地方，本片读规格后记录在此。

本文引用的规格文本全部来自 RISC-V ISA 手册仓库的 H 扩展一章，复现命令：

```sh
gh api repos/riscv/riscv-isa-manual/contents/src/priv/hypervisor.adoc \
  --jq '.content' | base64 -d > hypervisor.adoc
```

（本片用的是该仓库 `main` 分支当时的 `src/priv/hypervisor.adoc`，2943 行。异常
cause 编号见同文件的编号 cause 表：8 = from U/VU、9 = from HS、10 = from VS、
11 = from M、12/13/15 = insn/load/store page fault、14 与 17 是 Reserved、
22 = virtual instruction。本文 §2.2 提到的逐位表对不齐问题也出自这份文件。）

> **别把 scause 当十进制读。** 本轮实测到 `hyp: trap #1 scause=14 pc=0`（
> `.kernel-build/smoke/hyp-console-p0-riscv64.log`），紧跟着的是
> `hyp: guest instruction fetch fault gpa=0 pc=0 rc=-14`。**`scause` 是 `%lx` 打的，
> 所以 `14` 是 0x14 = 20 = guest 取指缺页**，正是 QEMU 10.0.13 的
> `RISCV_EXCP_INST_GUEST_PAGE_FAULT = 0x14`（`target/riscv/cpu_bits.h:720`），也正是
> 树里 `HYP_SCAUSE_GPF_INST`（`kernel/hyp/hyp_vcpu.c:71`）处理的那一条
> （`hyp_vcpu.c:810-822`）。同一份日志里还有 `scause=15`（0x15 = 21 =
> `RISCV_EXCP_LOAD_GUEST_ACCESS_FAULT`）与 `scause=17`（0x17 = 23 =
> `RISCV_EXCP_STORE_GUEST_AMO_ACCESS_FAULT`，`cpu_bits.h:721,723`）。
> **读成十进制会把 guest 取指缺页误判成规格里的 Reserved 14。** 这条 guest 为什么
> 跳到 `pc=0`，本片**没有定位**；它出现在 [03-usage.md §3.4.1](03-usage.md) 记录的
> 那次门禁失败里。

**（一）`HYP_HIDELEG_DEFAULT` 的三个位是只读零，写了等于没写。** —— **已修。**

原始定义（`kernel/include/hyp/hyp_vcpu.h`）是

```c
#define HYP_HIDELEG_DEFAULT (((uint64_t)1 << 1) | ((uint64_t)1 << 5) | \
                             ((uint64_t)1 << 9))
```

RISC-V H 扩展规格对 `hideleg` 的属性规定为（`riscv-isa-manual`
`src/priv/hypervisor.adoc`，"Hypervisor Trap Delegation"一节）：

> Among bits 15:0 of `hideleg`, bits 10, 6, and 2 (corresponding to the
> standard VS-level interrupts) are writable, and bits 12, 9, 5, and 1
> (corresponding to the standard S-level interrupts) are read-only zeros.

也就是说 bit 1/5/9 是 **S 级**中断位、只读零，bit 2/6/10 才是 VS 级可写位。
按原宏写下去，`hideleg` 读回来是 0，一个中断也没委托——而契约正文宣称的是
"guest 从 vs tvec 自己派发"。行为与契约相反。

QEMU 在软件里做同一件事，不靠 CSR 属性：`rmw_hideleg64` 用
`wr_mask & vs_delegable_ints` 逐位与（`target/riscv/csr.c:4658-4668`），而
`vs_delegable_ints = (VS_MODE_INTERRUPTS | LOCAL_INTERRUPTS) & ~MIP_LCOFIP`
（`csr.c:1779`），`VS_MODE_INTERRUPTS` 正是 2/6/10（`cpu_bits.h:790`）。掩码外的位
不是"写被拒绝"，而是**被 AND 掉**，所以读回 0 是静默的。

**当前状态**：已按 `(1<<2) | (1<<6) | (1<<10)` 修正（`hyp_vcpu.h:216-222`），
`hyp_arch.c` 里那句"nothing here is silently dropped"的注释也一并改正——原来
那句断言与 QEMU 源码相反。委托后硬件按规格把 VS 级 cause 10/6/2 翻译成
VS-mode 看到的 cause 9/5/1，所以 guest 侧不需要任何改动。

**（二）`hstatus.VTVM` 与"VS CSR 硬件直通"互斥。**
契约说（`hyp_vcpu.h:187-188`）

> VS-mode CSR accesses (satp/sie/stvec/...) hit the VS variants in
> hardware -- no interception, nothing to save while one guest runs.

这句要成立，`VTVM` 必须是 0。而已落地的 v1 架构半把 VTVM 设成了 1：

- `kernel/arch/riscv64/hyp/hyp_arch.c:111`
  `HYP_HSTATUS_GUEST_ON = SPV | SPVP | VTVM`
- `kernel/arch/riscv64/hyp/hyp_vcpu_asm.S:45` 同一掩码的汇编副本

规格对 VTVM 的规定是（`hypervisor.adoc`，`norm:hstatus_vtvm_op`）：

> When VTVM=1, an attempt in VS-mode to execute SFENCE.VMA or SINVAL.VMA or
> to access CSR `satp` raises a virtual-instruction exception.

只覆盖 `satp` / `SFENCE.VMA` / `SINVAL.VMA` 三样——`vstvec`/`vsie`/`vsscratch`
这些 VS CSR 本来就直接可访问，不受 VTVM 影响。但 `satp` 受影响，而 A20OS 的启动
路径一定会写 `satp`。`VTVM=1` 下的后果不是"陷回 HS 让 HS 模拟"，而是一次
**virtual-instruction exception（cause 22）**：委托与否它都落在 guest 自己的 trap
handler 上（除非 HS 把它留在 HS 侧处理，那正是本文不建议的路）。guest 的
`csrw satp` 因此变成一次它不认识的异常。

因此 v2 架构半必须把 `VTVM` 从 `HYP_HSTATUS_GUEST_ON` 里去掉（两处，C 与 .S
各一份）。这同时让 §3 的"无需保存"结论成立。

**当前状态：没有去掉，改走另一条路。** 工作树保留 `VTVM=1`
（`hyp_arch.c:166-167` 与 `hyp_vcpu_asm.S:47-53` 的 `HYP_HSTATUS_GUEST_ON` /
`HSTATUS_GUEST_ON` 都含该位），并在运行循环里模拟被 VTVM 拦下的那三条
（`kernel/hyp/hyp_vcpu.c:927-1015`，`hyp_emulate_virt_inst()`）：

- `csrr`/`csrw satp` → 转成 HS-mode 下的 `csrr`/`csrw vsatp`（CSR `0x180` ↔ `0x280`）；
- `sfence.vma` → 在 HS-mode 直接执行；
- `csrw vstimecmp` → 转成 `csrw 0x24d`（`vstimecmp`）。

`vsatp`/`vstvec`/`vsie`/`vsscratch` 这些 VS CSR 不受 VTVM 影响，仍由硬件直通。
代价是 guest 每写一次 `satp`/`sfence.vma` 多一次 HS 侧的解码与一趟 `sfence`；
换来的是架构半不必再动 `HYP_HSTATUS_GUEST_ON` 的既有形状，且 vstimecmp 的写入
路径（依赖 `henvcfg.STCE` 才不被 VTVM 拦）在 Sstc 缺失的平台上也有统一的落点。
**代价清单里还有一条未覆盖**：模拟器只认这三种编码，guest 若在 VS-mode 执行
别的虚拟指令（WFI 在 `VTW` 下、`HLV` 系列），仍是一次 cause 22；VTVM=1 时
`WFI` 不在 VTVM 的拦截范围内（它归 `VTW`，本片让 `VTW` 保持 0），所以现役
风险只有未被识别的 CSR 写与指令。

---

## 3. VS CSR 硬件直通，以及为什么无需保存

VS-mode 与 HS-mode 共享同一个 hart 的物理寄存器组，但**不共享 CSR**。规格给
每个 VS 级 CSR 准备了影子寄存器，VS-mode 里的普通名字被重定向过去：

| VS-mode 写的名字 | 实际落到 |
| --- | --- |
| `satp` / `sie` / `stvec` / `sscratch` / `sstatus` / `sepc` / `scause` / `stval` | `vsatp` / `vsie` / `vstvec` / `vsscratch` / `vsstatus` / `vsepc` / `vscause` / `vstval` |
| `sip` 的 SSI/STI/SEI 位 | `vsip` 的对应位 |
| `sret` | 回到 `vsstatus.SPP` 指定的虚拟态 |

规格原话（`hypervisor.adoc`，"Virtual Supervisor Address Translation and
Protection (`vsatp`) Register"）：

> `vsatp` substitutes for the usual `satp`, so instructions that normally
> read or modify `satp` actually access `vsatp` instead.

**"无需保存"的准确含义**：只要 `VTVM=0`、且 HS 不需要读 guest 的 `vs*` CSR，
这些寄存器**在 guest 进出之间原样留着**，HS 根本不碰它们——没有 swap、没有
进出钩子、没有 shadow 区。这与 v1 的 "帧即状态" 并不冲突：v1 要存的是**通用
寄存器组**（共享，必须存），v2 免掉的是**VS 影子 CSR**（不共享，不必存）。二者的
接口仍然是同一个 `hyp_vcpu_handle_trap()`。

这条结论有两个必须写下来的前提，都还没在代码里落地：

1. **`VTVM` 必须为 0**（§2.3）。`SPVP=1` 保持不变——它是 HS 侧唯一能合法读
   `vs*` 的授权位。
2. **`hcounteren.TM` 必须置 1。** A20OS 的 `timer_get_ticks()` 直接读 `time`
   CSR（`kernel/arch/riscv64/platform/timer.c:58-62`）。规格
   （`norm:hcounteren_op`）规定 `hcounteren` 的位清零时，低于 HS 的特权级读
   `cycle`/`time`/`instret` 会触发异常；`hcounteren` 必须实现，具体位是 WARL。
   顺带：guest 的 `timer_init()` 自己会写 `scounteren |= 0x2`（`timer.c:39-42`），
   而 VU 模式读 `time` 需要 `hcounteren` 与 `scounteren` **同时**置位
   （`norm:hcounteren_scounteren_vu_op`），所以 `hcounteren.TM` 是硬要求。

`time` 本身**不陷到 HS**：规格（"Hypervisor Time Delta (`htimedelta`)
Register"）规定 VS/VU 模式读 `time` 返回 `htimedelta + time`。`htimedelta`
留在 0 就意味着 guest 与宿主共用同一个单调时钟，也意味着**不需要为时钟做任何
per-vcpu 状态**。

---

## 4. SBI 面：按 guest 实际会发出的调用面

guest 是 A20OS 自己，它的 SBI 调用面由
`kernel/arch/riscv64/platform/firmware.c` 与
`kernel/arch/riscv64/platform/timer.c` 决定，不是由"hypervisor 想支持什么"
决定。下表是逐个读这两个文件得到的**实际会发出的调用**，EID 取自
`kernel/arch/riscv64/include/firmware.h:6-16`。

| 发出方 | a7 = EID | a6 = FID | a0 | v1 现状 | v2 必须 |
| --- | --- | --- | --- | --- | --- |
| `firmware_console_putchar` | `0x01` | 0 | 字符 | 已实现（`hyp_vcpu.c:390-393`） | 改为走 16550 设备模型 |
| `firmware_console_getchar` | `0x02` | 0 | — | **未实现** | 允许，恒返回 -1（无输入） |
| `firmware_set_timer` | `0x00` | 0 | deadline | **未实现** | 必须，或让 guest 走 sstc（见下） |
| `firmware_shutdown` | `0x53525354` (SRST) | 0 | 0 | **未实现** | 必须 |
| `firmware_reboot` | `0x53525354` (SRST) | 0 | 1 | **未实现** | 建议实现为等价 shutdown |
| `sbi_send_ipi` | `0x735049` | 0 | mask, base | 未实现 | 单 vcpu 下用不到 |
| `sbi_hart_start` | `0x48534D` (HSM) | 0 | hart, addr, opaque | 未实现 | 单 vcpu 下用不到 |
| `sbi_remote_sfence_vma` | `0x52464E43` (RFENCE) | 1 | mask, base, start, size | 未实现 | 单 vcpu 下用不到 |

**v1 与 v2 最大的行为断裂在这里。** v1 的冻结契约
（`hyp_vcpu.h:136-141`）只服务 `a7=0x01` 与 `a7=0x08`，而 `0x08` 是 legacy SBI
的 shutdown EID；当前 `firmware_shutdown()` 走的是 SRST 扩展
`0x53525354`（`firmware.c:50-52`），`SBI_SHUTDOWN_EID 8` 在
`firmware.c` 里已无使用者。也就是说：**v1 的 demo guest 能关机，是因为它的字节码
是手写的、只发 `a7=0x08`；真正的 A20OS 内核不会发那个数**。v2 必须按上表实现，
`0x08` 只能作为向后兼容的别名保留。

**`set_timer` 不是可选项。** `timer_set_interval()`（`timer.c:46-56`）只有在
`riscv64_fdt_has_isa_extension("sstc")` 为真时才写 sstc 的 stimecmp CSR，否则
调 `firmware_set_timer()`。而 `riscv64_fdt_has_isa_extension()` 查的是 FDT 里的
`riscv,isa` 属性（`kernel/arch/riscv64/platform/fdt.c:116`）——最小 FDT 里
若没有这一项，guest 一律走 SBI `set_timer`。此时 guest 自己没有任何办法给自己
设定时器，除非 HS 实现 `a7=0`。**把 sstc 写进最小 FDT 与实现 `set_timer` 两条路
至少要有一条通**；这是引导切片必须同时面对的两件事，不是可以推后的细节。

> 关于 ecall 的 cause 编号：契约写的"ecall-from-VS (scause 10)"是对的。
> 规格的异常 cause 表里 8 = from U/VU、9 = from HS、**10 = from VS**、
> 11 = from M。v1 的 `hyp_vcpu.c:44-46` 三个都收，属于无害的超集。

---

## 5. 二级缺页路由

一次二级缺页（`htval` 给出 GPA）按三段裁决，顺序不可换：

| 顺序 | 判据 | 动作 |
| --- | --- | --- |
| 1 | GPA 已被 `hyp_vm_load()` 映射，或已在 RAM 窗口内被填充过 | 表里有映射就是表里的；无映射则走 2 |
| 2 | GPA ∈ `[ram_base, ram_base + ram_size)` | `hyp_ram_fill()`：分配**一帧清零内存**，`hyp_s2_map()` 以 RWX 装入，guest 重试同一条指令即命中 |
| 3 | 其它 | 交给 `hyp_dev_mmio()`；返回 0（地址不认识）则以 `HYP_EXIT_FAULT` 退出并留下 `exit_htval` |

- **窗口默认 `[0x80000000, mem_size)`**（`HYP_RAM_BASE_DEFAULT`，`kernel/hyp/hyp.c:24`，
  赋值在 `hyp.c:115-116`）。这与 guest 板的
  内存布局一致：`kernel/arch/riscv64/include/platform.h:14-16` 把 QEMU virt 的
  `PHYS_MEMORY_BASE` 定为 `0x80000000`，启动代码链接在
  `KERNEL_ENTRY = 0x80200000`（`:20`）。
- **窗口页必须是 RWX**，且 `PTE_U` 必须置位（`PTE_U` 的理由见
  [00-design.md §5.3 支撑改动](00-design.md)）。**已落地**：工作树里
  `HYP_RAM_PAGE_PROT` 是 `PTE_R|PTE_W|PTE_X`（`kernel/hyp/hyp.c:50`），
  `hyp_vm_load()` 装镜像用的 `HYP_GUEST_PAGE_PROT` 是同一个值
  （`kernel/hyp/hyp_vcpu.c:100`），`hyp_s2_map()` 在 prot 之上再补 `PTE_R|A|D|U`
  （`kernel/hyp/hyp.c:229`）。理由与被推翻的旧结论见
  [00-design.md §5.3 第 3 条](00-design.md)：R|W|X 在 QEMU 10.0.13 的
  `get_physical_address()` 里落到 `PAGE_READ|PAGE_WRITE|PAGE_EXEC`，正常翻译，
  不是保留编码。**已验证**：本轮 `smoke-hyp-a20os` 里 guest 一路走到
  `[RAMFS] Initialized`，其中包含大量对只读装入页的写（`.data`、`.bss`、页表、
  栈）——R|X 的镜像在这条路上早就 fault 了。
- **窗口页与预装页的关系**：`hyp_vm_load()` 装过的页是显式映射，不会触发缺页；
  窗口只覆盖 guest 自己分配的内存。两者重叠时以先建立的映射为准
  （`hyp_s2_map()` 对已映射 GPA 返回 `-EEXIST`）。
- **记账**：`hyp_ram_fill()` 记到当前任务的 cgroup。树里已有的接口是
  `cg_mem_charge()` / `cg_mem_uncharge()`（`kernel/mm/vmo.c:276,284`），
  与 VMO 记的是同一本账——这正是 capability 叙事里"guest 内存就是宿主内存、
  只是被出借"的落点（见 §9）。
- **"静默按需补页"这条禁令已被一条可判定的规则替代**：窗口内的 GPA 补页并让
  guest **重试同一条指令**，窗口外交给设备模型并由宿主**代执行这条指令**，
  补页失败就是一次记下来的 fault——三条都在 `hyp_guest_mem_fault()` 的注释与
  实现里（`kernel/hyp/hyp_vcpu.c:780-789` 的两出口说明、`815`、`843-848`、
  `851-857`、`866-872`）。契约头还留着"no silent on-demand fill"那句
  （`kernel/include/hyp/hyp_vcpu.h:154-155`），指的是"补页不能藏在 trap 里悄无声息地
  进行"，与这里写的是同一件事。

---

## 6. 设备模型行为表

设备模型归 `kernel/hyp/hyp_dev.c`（**进行中**：本文写作时该文件在工作树里、
未提交），对外只有 `hyp_dev_mmio(vm, gpa, store, *value, len)` 一个入口。下表
左列是 guest 侧 A20OS 真实的访问来源，右列是模型该做的事——"该做"不是猜的，
是照着这些访问点反推的；在途实现与本表的差异已就地标出。

一条贯穿全表的设计约束（`hyp_dev.c` 文件头）：设备模型运行在 **guest trap
路径**上，guest 停在 HS-mode、trap 帧还活着，调用者马上就要 `sret`。所以这里
不取任何锁、不睡眠、不分配；所有共享状态走 `__atomic`。

### 6.1 16550 UART `[0x10000000, 0x10001000)`

guest 的 UART 代码在 `kernel/arch/riscv64/include/console.h`（内联头，全部寄存器
访问都在这里）。

| 寄存器 | 偏移 | guest 侧做什么 | 模型该做什么 |
| --- | --- | --- | --- |
| `IER` | 1 | `arch_uart_init()` 写 0x00 → … → 0x01（`console.h:18-25`） | 静默丢弃，记写入 |
| `LCR` | 3 | 写 0x80 再写 0x03（8N1，去 DLAB） | 静默丢弃 |
| `FCR` | 2 | 写 0x07（`console.h:23`） | 静默丢弃 |
| `MCR` | 4 | 写 0x0B | 静默丢弃 |
| `THR` | 0 | `arch_uart_putc()` 自旋等 LSR，再写字节（`console.h:27-32`） | 写到宿主 console，计入 guest console 字节数，逐字节比对 marker |
| `RBR` | 0 | `arch_uart_poll_getc()` 先读 LSR（`console.h:34-39`） | **已实现**：读弹出 ingress ring 里最老的一个字节，空环回 0（`kernel/hyp/hyp_dev.c:290-304` 的 `HYP_UART_THR` 分支，`hyp_uart_pop_rx()` 在 `:269-279`）。多字节读只认第一字节（`byte_index != 0` 不弹） |
| `LSR` | 5 | `arch_uart_putc()` 等 bit `0x20`；`arch_uart_flush()` 等 bit `0x40`；`arch_uart_poll_getc()` 读 bit `0x01` | **返回 `0x60` 或 `0x61`**：`0x60` 恒在（`HYP_UART_LSR_TX_EMPTY`，`hyp_dev.c:69`），bit `0x01`（DR）由"ring 非空"决定（`hyp_dev.c:287-288`；`hyp_uart_rx_ready()` 是**窥视不消费**，`:262-267`） |
| `IIR` | 2 | 通用串口驱动会轮询中断标识 | 读 `0x01`（"无中断待处理"，`HYP_UART_IIR_NONE`，`hyp_dev.c:76,286`）。**在途实现补了这一条**：RAZ 会宣称有一个永远不会被投递的中断，那是唯一能把轮询 IIR 的驱动挂死的值 |
| `MSR` | 6 | 等 CTS/DSR 再写的驱动 | 读 `0x30`（DSR+CTS 置位，RLSD 不置，`HYP_UART_MSR_CARRIER`，`hyp_dev.c:79,289`）。**在途实现补了这一条** |

`LSR = 0x60` 是**承重值**，不是随手填的：契约（`hyp_vcpu.h` 的 v3 一节，
`kernel/include/hyp/hyp_vcpu.h:250-262`）给的就是 `0x60`，并明确说**只有 DR 位是
后加的**。bit `0x20`（THRE）必须常置，否则 `arch_uart_putc()` 里的
`while ((uart[5] & 0x20) == 0);` 会把 guest 焊死在一个自旋里——这是启动冒烟
最可能卡住的地方。bit `0x40`（TEMT）同理，`arch_uart_flush()` 会等它。bit
`0x01` 从"恒 0"变成"有输入才 1"，这正是契约 v3 带来的唯一寄存器语义变化。

**中断**：`arch_uart_init()` 打开了 IER bit 0，`uart_init()` 又无条件
`request_irq(UART0_IRQ=10, …)`（`kernel/drivers/char/uart.c:184`）。v2 不路由
中断（契约明说"interrupts are delegated, not routed"），所以 source 10 永远不会
被断言。树里已经有现成的出路：`board_config_t.uart_rx_is_polled`
（`kernel/drivers/core/driver_core.h:309`）正是给"UART IRQ 路由与定时器陷阱都
不可用"的板子准备的，已有 LS2K1000 / VisionFive 2 / loongarch QEMU virt 三家
在用。**v2 guest 板应当置这个位**，否则 guest 会挂一个永远不来的中断。

### 6.2 CLINT `[0x02000000, 0x02010000)`

| 寄存器 | guest 侧做什么 | 模型该做什么 |
| --- | --- | --- |
| `mtime` | **当前 riscv64 路径不读它**（`timer_get_ticks()` 读 `time` CSR，`timer.c:58-62`；`kernel/drivers/irqchip/plic.c` 也不碰 CLINT） | 读返回宿主 timebase，写丢弃 |
| `mtimecmp` | 只有 `vstimecmp`/sstc 路径会到（`timer.c:49-52` 写 CSR `0x14d`） | RAZ/WI |
| 其它 | — | RAZ/WI |

契约要求建模 mtime 读的理由是防止 guest 时钟冻结（`hyp_vcpu.h:166-167`）。事实
是：**今天的 A20OS guest 走 `time` CSR，不走 CLINT MMIO**，所以这条模型当前是
防御性的。保留它有两个好处：一是 guest 的启动/探测代码未来可能读 CLINT，二是
`hcounteren.TM` 一旦被某个平台实现成只读零，MMIO 路径就是唯一的退路。

### 6.3 其余一切 RAZ/WI + 每页计数器

PLIC 在 `0x0C000000`（`platform.h:46`），virtio-mmio 从 `0x10001000` 起
（`:45`），PCIe ECAM 在 `0x30000000`（`:47`）——全都在窗口外，全部落进这一档。

PLIC 特别值得说一句：`plic_configure(PLIC_PLIC_BASE, …)` 在 guest 的
`early_init` 里是**无条件调用**的（`kernel/platform/qemu-virt-riscv64/board.c:171`），
所以 guest 一启动就一定会去写 PLIC 的 PRIORITY/SENABLE 寄存器。RAZ/WI + 计数器
保证这些写不卡死，也不至于让 guest 在一个"IRQ 永远不会来"的等待里停住。

---

## 7. 引导协议

### 7.1 入口约定

`hyp_vcpu_set_boot(vcpu, hartid, dtb_gpa)`（`hyp_vcpu.h:196`）把两个值放进
guest 的 a0/a1。guest 侧的解读在 `kernel/arch/riscv64/boot/entry.S:88-90`：
a0 存进 `__boot_hart_id`，a1 存进 `__boot_dtb_ptr`。与 OpenSBI 的约定一致，
也意味着**同一个 kernel.elf 既是宿主内核也是 guest 内核**，不需要为 guest 单独
构建——这是"把 A20OS 自己当 guest"最省事的一处。

单 vcpu 切片下 hartid 只能是 0。a0 != 0 会走 `entry.S` 的 secondary 分支，
那里要 `sbi_hart_start()` 与 `riscv64_smp_release`（`entry.S:37-61`），两者在
v2 的 SBI 面里都不存在——这条路径必须先失败得干净，不能半启动。

### 7.2 镜像怎么进 guest RAM

契约里**没有 ELF 装载接口**。可用的只有 `hyp_vm_load()`（整页复制）与
`hyp_ram_fill()`（清零填充），所以 ELF 的解析只能在宿主侧做。**进行中**：
`user/cmds/core/hyp_boot.c` 选了用户态装载器这条路——从宿主文件系统读
guest ELF，按 `PT_LOAD` 段逐段 `hyp_vm_load()` 到各段的 **LMA**。

**装载地址是 LMA，不是 `e_entry`**。这是这半最容易错的一处：
`kernel/arch/riscv64/boot/ldscript.ld:17-18` 把内核链成高半区
（`PHYS_BASE = 0x80200000`、`VIRT_BASE = 0xFFFFFFC080200000`），每段的 LMA 1:1
落在 VMA 之下，所以 `e_entry` 是个虚拟地址，不是 guest 物理地址。QEMU 的
`-kernel` 装载器跳的是 LMA，`hyp_boot` 也是：入口 GPA 是 `e_entry` 经 program
header 换算出来的 LMA，落在 `0x80200000` 一带（`platform.h:20` 的
`KERNEL_ENTRY` 就是这个数）。`entry.S` 自己把启动页目录铺成
`BOOT_MAP_PHYS..+16 GiB` 的 identity 映射，guest 的链接地址不能变；它还有
`.done_bss:` 标签会自己清 bss，所以装载器不必也不该替它做这件事。

### 7.3 最小 FDT 要有什么

guest 侧对 FDT 的全部依赖有四处，逐个对着代码看：

| 需要的东西 | 谁在读 | 缺了会怎样 |
| --- | --- | --- |
| 合法 FDT 头 | `fdt.c:194`、`fdt.c:205` | 打印一行告警后回退到板级窗口 |
| `/memory` 的 `reg` | `riscv64_memory_init()`，`fdt.c:262` | 回退到板级窗口 `0x80000000 .. +16 GiB`（`platform.h:16,19`）——**这条绝不能缺、更不能解析失败**：`hypvm -m` 默认只给 128 MiB，回退值比窗口大 128 倍，guest 按回退值分配就立刻越窗（曾经的 `kfree(magic=0x0)` 就是这条链） |
| `/cpus` 的 `riscv,isa` 含 `sstc` | `timer.c:30` | guest 改走 SBI `set_timer`（见 §4，这是个必须补的洞） |
| `/chosen/bootargs` | `arch_bootargs_get()`，`fdt.c:301` | 返回 NULL，命令行参数全丢 |

所以最小 FDT 的必需项是：**合法头部 + 内存节点 + 一个 `chosen/bootargs` 节点**。
内存节点**必须给、而且必须真的解析成功**——回退值 `+16 GiB` 远大于 `hypvm -m`
给的窗口，guest 拿回退值去分配就会越窗。**当前树的合成 FDT 是解析成功的**（实测
`[FDT] RAM range 0x80000000..0x88000000 (128 MiB)`、`bootargs='a20.hypguest=1'`，
`grep -cE 'memory node parse failed|using board window'` = 0）；曾有一版因为合成
blob 的 4 字节 padding 写错，整个结构块失步、四条属性同时读不到，见
[03-usage.md §2.3](03-usage.md)。`sstc` 给不给决定要不要实现 SBI
`set_timer`——建议给，两条都通最省心。FDT 放在哪由装载器决定，但 §7.4 有
一个硬约束必须满足。

### 7.4 FDT 指针的一个硬约束

`__boot_dtb_ptr` 被当作**指针直接解引用**（`fdt.c:193`、`fdt.c:77`），
`entry.S:90` 存的是 a1 原值。SBI 约定里 a1 是**物理地址**，而 guest 一旦装上
自己的页表，物理地址就不再是可直接解引用的虚拟地址。

v2 的 guest 是会装页表的（A20OS 的启动路径一定会写 `satp`），所以：

- 要么把 FDT 放在 guest 装页表之后仍然 identity 可达的地方，
- 要么改 guest 侧，让它在建表后把 `__boot_dtb_ptr` 走一次 `va_to_pa`。

**v1 的 demo guest 没有这个问题**（`vsatp` 停在 Bare，VA 就是 GPA）。这是 v2
引入的第一个"真实 guest 才暴露"的坑，本片只记录，未验证。

---

## 8. 冒烟判据：marker，不是日志交叉比对

### 8.1 为什么不用日志

v1 的 gate 靠两行 console 输出交叉比对：`^HYP$` 与 `HYP_VCPU_TEST: PASS`
（`tools/smoke_cases.py:526-534`）。那是可行的，因为 v1 的 guest 只打三个字符。
v2 的 guest 会打出整个内核启动日志，而宿主自己的日志与之共享同一个 console
通道：两者的行会交错，`[FDT]` 这类前缀在两边都可能出现。这种判据在日志变长
之后不再是判据。

### 8.2 marker 机制

契约给了三个入口（`hyp_vcpu.h:198-206`）：

- `hyp_vm_set_marker(vm, marker)`：≤ 32 字节。设备模型在**每一个** guest UART
  字节上做匹配。
- `hyp_vm_marker_seen(vm)`：命中与否。
- `hyp_vm_console_bytes(vm)`：自 run 开始以来的 guest console 字节数。

判据因此是三个独立事实，而不是"日志长这样"：

| # | 断言 | 它排除什么 | 实现状态 |
| --- | --- | --- | --- |
| 1 | `hyp_vm_marker_seen(vm) == 1` | guest 走完了从入口到打印 banner 的整条路——包括委托的异常处理、按需 RAM 窗口和 16550 的 THR 路径 | **已实现**（`hyp_boot.c:218-219`） |
| 2 | `hyp_vm_console_bytes(vm) > <marker 长度>` | marker 不是"只匹配到开头就停"的退化命中 | **已实现**（`hyp_boot.c:221-223`） |
| 3 | `hyp_vcpu_run()` 返回 0 且 `vcpu->exit == HYP_EXIT_SHUTDOWN` | guest 是自己关机，不是撞死 | **未实现**，且**这条判据本身与 PASS 的定义冲突**：本门禁的 PASS 是"guest 打到了自己的 banner"，不是"guest 正常关机"。当前代码只检查 `exit_reason >= 0`（`hyp_boot.c:201`），退出原因只打出来给人看（`:205-212`），不参与判定。**本轮这条尤其不能加**：实测那次 `exit=1(shutdown)` 恰恰是 guest panic 之后发的（§8.3），把它当成功判据会把一次 panic 判成正常关机 |
| 4 | `mm_s2_audit(vm)` 返回 0 | 出借的帧与 stage-2 表在 guest 死后仍然对得上 | **未实现**：内核侧有 `hyp_s2_audit()`/`mm_s2_audit()`（`kernel/hyp/hyp.c:275`），但没有任何 syscall 能从用户态取到它的结果 |

第 1 条已经包含了"这份输出来自 guest 通道"这层意思——marker 只在 guest UART
字节上比对，宿主自己打的同名 banner 不会让它命中。这一点很要紧，因为宿主和
guest 是同一个内核，**会打出字节完全相同的一行**。

### 8.3 该带什么 marker，以及冒烟的边界

用 guest 自己最早打印的一行、且这行只可能在"启动真的走通"时出现的一行。理由
是 marker 的强度等于它所证明的事情的强度：打在启动早期的短串只证明"进了
C 代码并能写串口"，打在内存子系统就绪之后的串才顺带证明窗口填充成功。
**进行中**：`user/cmds/core/hyp_boot.c` 选了 `"A20OS Kernel"`，也就是 guest
最早的 banner 行。

**PASS 的定义是"guest 到达了自己的 banner"，不是"guest 引导完成"。**
这轮门禁构建的是 `RAMFS_USER=0` 镜像（`tools/smoke_cases.py:630`），guest 没有块
设备、挂不上 rootfs，会死在 `init_kthread` 里——那是真实到达，不是失败。
**当前实测的终点正是那里**：`smoke-hyp-a20os` 当前日志（632 行）里 guest 走完
banner、`[INIT] entering scheduler...`、driver manager，停在
`[INIT] Cannot open /bin/init: -2` → `init: no init program found` → `KERNEL PANIC`
→ `HYP_A20OS: PASS`。**更早两轮的终点更靠前**（slab 层的 `kfree`、进调度器时的
栈页 use-after-free），两个根因都已修（§8.3 第五次）。这条边界必须写进 gate，否则两件坏事之一必然发生：

- 把 `PANIC` 放进 forbid：guest 的 panic 文本与宿主的**逐字节相同**，一条正确
  的运行会被判红；
- 不写清楚 PASS 的含义：读者会以为 gate 覆盖了 guest 的完整启动。

**进行中**：`tools/smoke_cases.py` 的 `smoke-hyp-a20os` 按前一种取舍办——
`forbid` 刻意**不含** `PANIC`，只列 `LOCK-STALL`、`MCS DEADLOCK`、
`HYP_A20OS: FAIL`；宿主真的死掉由"缺了 `HYP_A20OS: PASS` 期望"抓住，因为
verdict 行在宿主活着的时候才写得出来。这条 gate 的 timeout 是 300 s 而不是
60 s：每个 guest console 字节、每次页表走查、每次二级缺页都要陷回宿主，
guest 引导比宿主自举慢好几个数量级。

**guest 在 banner 之后走到哪儿，与判据无关。** 判据只看 marker，不看退出原因。
五次实测（前三次是 `.kernel-build/smoke/hyp-a20os-riscv64.log`，第四、五次是
`hyp-shell-riscv64.log`），五次门禁都 PASS——**但第五次是 guest 真的通了，
前四次是 marker 与 guest 崩溃同时出现**：

**第一次（日志可读，全文 573 行）**，guest 走到根 ramfs 就绪，然后：

```
556: [RAMFS] Initialized, root inode 0
557: [SLAB BUG] kfree(0xffffffc47fae0010): invalid non-slab pointer …
571: [PANIC] attempting firmware poweroff
572: HYP_A20OS: exit=1(shutdown) scause=0xa stval=0x0 htval=0x0 marker_seen=1 console_bytes=17840
573: HYP_A20OS: PASS
```

**`exit=1(shutdown)` 与 `PASS` 同时出现，中间隔着一次 guest 自己的
`KERNEL PANIC`。** `shutdown` 只说明 guest 发了一次 SBI SRST ecall
（`hyp_vcpu.c:413-418`），那次是 panic 路径在打完堆栈之后发的。`[SLAB BUG]`
是 **guest 自己 slab 层报的**（guest 用的是同一棵树的 slab 代码）。**当时本片写的是
"没有定位根因"，现在已定位**：合成 FDT 的 4 字节 padding 失步 → guest 回退板级
16 GiB 窗口 → 越窗分配 → RAZ 喂零 → `kfree(magic=0x0)`，已修，见 §8.3 第五次。
同一个日志里还有一条对照：
`292: [BUS] virtio-mmio: found 0 devices`——guest 在自己的 stage-1 里数到了两个
virtio-mmio（宿主那次是 `:103` 的 2 个），但设备模型给的是 RAZ/WI 计数，不是真设备。

**第二次（日志被在途调试打印刷成 43 万行，guest 段读不出）**：结果行逐字段相同
（`console_bytes` 也是 17840），可 guest 侧换成停在 UART 页上的自旋
（`hyp: trap loop at pc=ffffffc080209664 scause=17`），而且 `[SLAB BUG]` 与
`KERNEL PANIC` 这两行**不再出现在日志里**——原因不是被修好了，是
`kernel/hyp/hyp_vcpu.c` 两处在途 `DBG` 打印把日志撑到了 432,618 行。
**"这次没看见"不等于"这次没有"。**

**第三次（去掉调试打印后，那一轮的树，PASS，日志 573 行可读）**：**和第一次停在
同一个地方。**
`[RAMFS] Initialized, root inode 0` 之后紧跟

```
[SLAB BUG] kfree(0xffffffc47fc20010): invalid non-slab pointer hdr=0xffffffc47fc20000 magic=0x0 order=0
[SLAB BUG]   pfn=4193312 flags=0x1 refcount=1 order_meta=5 cpu=0
[FRAME-TRACE] releases of pfn=4193312:
```

**所以第二次的自旋是那两条 `DBG` 造成的假象；到第三次为止，guest 的真实终点是
`kfree` 一个 RAMFS 指针**（第四次已经不是了，见下面那条引注）。 `flags=0x1 refcount=1` 说明那一页在 buddy 层仍是"已分配"
的——被 `kfree` 的地址落在 `0xffffffc47f...`，正是 guest 的 RAMFS 区间，ramfs 把
自己持有的页当普通页还了回去。**这不是 stage-2 的问题**：解码零缺口
（`grep -c "undecodable guest access"` = 0）、guest 一路走到 `[INIT] USB devices
scanned`（17840 个 guest console 字节）、该走的二级页错配都走完了。同一现象在
`RAMFS_USER=1` 的内核上**一模一样地复现**（同一个 `[RAMFS] Initialized` 后面、
同一个 `[SLAB BUG]`，只有 `pfn` 差了一点），所以也不是"guest 缺 `/bin/init`"那条路
导致的。

所以这条边界要写成两句话而不是一句：**PASS = guest 到达了自己的 banner；
`exit=` = guest 最后停在哪。** 第二次实测里 `exit=` 与 `console_bytes` 与第一次
完全一样而 guest 的实际终点不同（当时读不出来），就是这条边界最硬的证据。读日志要
读到最后一行之上最近的 `[PANIC]` / `[SLAB BUG]` / `trap loop`，而不是读 `PASS`。
逐行解读见 [03-usage.md §3.1-3.3](03-usage.md)。

> **上面这一段的"真实终点"已经被推翻了两轮，别照抄。** 第一次的终点（`kfree`
> 一个 RAMFS 指针）被第四次推翻；第四次的终点（内核栈页 use-after-free）被第五次
> 推翻。第四次的记录保留如下，**它是历史，不是现状**：那一轮
> `.kernel-build/smoke/hyp-shell-riscv64.log` **一次 `[SLAB BUG]` 都没有**
> （`grep -c 'SLAB BUG'` = 0），guest 走得更远——打完 banner、走到
> `[INIT] entering scheduler...`——然后死在**内核栈页的 use-after-free**
> （`[TRAP] pfn 32638 sits on buddy free list — use-after-free of stack page`，
> `[PANIC] task: pid=1 name=kthread`）。**"这不是 stage-2 的问题"仍然成立**
> （解码零缺口），但"guest 的真实终点是那个 kfree"当时就不成立了。
>
> **第四次（622 行日志，`smoke-hyp-a20os` 与 `smoke-hyp-shell` 同形）**：
> `554: [RAMFS] Initialized` → VFS / 四个 `DRVMOD` / LWIP → `578: [INIT] System ready`
> → `605: Welcome to A20OS!` → `607: [INIT] entering scheduler...` → `608` 起
> `[TRAP] corrupted kernel sp` → `KERNEL PANIC` → `[PANIC] attempting firmware poweroff`
> → `HYP_A20OS: exit=1(shutdown) … console_bytes=20926` → `HYP_A20OS: PASS`。
> `grep -c 'SLAB BUG'` = 0、`grep -c 'trap loop'` = 0、
> `grep -c "undecodable guest access"` = 0。逐行解读见
> [03-usage.md §3.3 第四次](03-usage.md)。
>
> **第五次（当前，`make smoke-hyp-shell` 绿，同路径日志现为 641 行）**：guest
> 走完上面全部步骤**再往下**——`[INIT] user init created: pid=3` →
> `[init] fork=0, calling execve mksh` → 提示符 → 门禁敲入的
> `echo AAAABBBBCCCC` / `echo DDEEEEEEFFFF` 两个 token 原样回显（日志 630-633 行）
> → `exit` → `[init] shutting down`（635 行）→
> `HYPVM: exit=1(shutdown) … console_bytes=21705 rx_bytes=41` → `HYPVM: PASS`
> （640-641 行）→ 宿主 `poweroff`。`grep -cE 'SLAB BUG|KERNEL PANIC'` = 0、
> `grep -cE 'memory node parse failed|using board window'` = 0、
> `[FDT] RAM range 0x80000000..0x88000000 (128 MiB)`（263 行）、
> `[FDT] bootargs='a20.hypguest=1'`（280 行）。两个根因（合成 FDT 的 4 字节
> padding 失步、guest 入口帧 `mstatus.FS=Off` 打穿栈）与修法见
> [00-design.md 的门禁表](00-design.md)；逐行对账见
> [03-usage.md §3.3 第五次](03-usage.md)。


**另有一条门禁 `smoke-hyp-console-p0`（`tools/smoke_cases.py:572-611`）**，它验的
不是这个 marker，而是本轮新增的 guest 控制台输入（契约 v3）：往 QEMU 的 stdin 里
按 `sendline_seq` 的顺序送三步（提示符 → `hyp_test echo` → guest up → 三个字节 →
提示符 → `poweroff`），期望日志里出现独立一行 `HYP`（guest 的回显）与
`rx_bytes=N`。**本片实跑结果：门禁 PASS。** guest 轮询 LSR、弹出三个宿主字节、
经 SBI 打回，控制台上是干净的 `HYP`，随后 SBI shutdown：
`HYP_VCPU_TEST: exit=1 rx_bytes=4` → `HYP_VCPU_TEST: PASS`。

> **它一度是红的，而红的不是通道。** 第一版 `hyp_test.c` 里
> `memcpy(code + n, guest_echo, sizeof(guest_echo) / sizeof(guest_echo[0]))`
> 把**元素个数当成了字节数**——拷了 13 字节（三个整字加第四个字的头一个字节）。
> guest 于是把 `andi a1,a1,1` 取成 `0x00000093`，解成 `addi ra,x0,0`，带着过期的
> LSR 字节跳进镜像后面的零填充，在 `pc=0` 取指失败。**这条编译不报、运行不报、
> 头一个字还对**，四步之外的 `trap #1 scause=14 pc=0` 是它唯一的表现，而那四步
> 全都"看起来对"——解码出的 `va`、`gpa`、`rd` 一项不差。

**第三条门禁 `smoke-hyp-console`（`tools/smoke_cases.py:742-808`）** 是端到端那条：
用 `-k /bin/boot/guest-kernel-ramfs.elf` 引导 `RAMFS_USER=1` 的 guest 内核
（userland 直接链进镜像，因此背后没有块设备也有 `/bin/init` 可 exec），并要求
`HYPVM:` 行上的 `rx_bytes` 非零。**实跑 PASS，`rx_bytes=20`**——`echo roundtrip` 的
15 个字节加 `exit\n` 的 5 个。`rx_bytes` 是设备模型自己的入口计数器，只在
`hyp_dev_pump_rx()` 里、只在真的从 `uart_try_getc()` 拿到字节时递增，而 pump 只在
guest trap 上跑，所以宿主伪造不了它。**这条门禁的判据本身不声称 guest 把它们读回来
了**——虽然当前实跑里 guest 确实走到了 mksh 并把 `roundtrip` 打了出来（日志
630-631 行），判据没变；要断言"guest 读了"看第四条。

**第四条门禁 `smoke-hyp-shell`（`tools/smoke_cases.py:810-962`，
`tools/targets-smoke.mk:202-213`）** 是"guest 能进 shell 并且能交互"那条目标的
验收点，第三条覆盖不到的那一半就是它。**实跑 PASS（当前）。** 它在第三条的
`rx_bytes` 之外**另外**要求：guest 走到 mksh 提示符、执行门禁敲进去的
`echo AAAABBBBCCCC` 与 `echo DDEEEEEEFFFF`、把两个 token 打印回来、回到提示符，
最后由 `exit` 触发 `user/init.c:264-283` 的 `do_shutdown()` 收尾。
**"字节进了环"和"guest 读了"是两条分开的断言**——第三条只断言了第一条。
当前实测：`rx_bytes=41`（两条 `echo` 加 `exit` 共 18+19+4 字节），两个 token
都在 guest 区原样回显，`grep -cE 'SLAB BUG|KERNEL PANIC'` = 0（日志 630-641 行，
见上文"第五次"）。**它此前是红的**，红点与根因也见"第五次"；门禁本身一度还会
因为死锁而假红（健康 guest 敲 `exit` 前不自退，原 steps 等 `HYPVM: PASS` 才发
`poweroff`，实测 300 s 超时），已把 guest `exit` 加为独立一步
（`tools/smoke_cases.py`）。逐行对账见 [03-usage.md §3.4.2](03-usage.md)，
路线图上的状态见 [02-roadmap.md](02-roadmap.md)。

> **命令行入口的行前缀是 `HYPVM:`，不是 `HYP_A20OS:`。** 本节与
> `tools/smoke_cases.py` 记的是硬编码装载器 `user/cmds/core/hyp_boot.c` 的
> `HYP_A20OS:` 前缀。带参数的 CLI 工具 `hypvm` 另起一套 `HYPVM:` 前缀的行，
> 其参数、DTB 放置规则、输出逐行含义、限制与排查表见
> [03-usage.md](03-usage.md)。门禁 `smoke-hyp-a20os` 断言的仍是装载器那条
> `HYP_A20OS: PASS`；`hypvm` 的判据行是 `HYPVM: PASS`。集成时若两者行文另有
> 出入，以实现为准并回来改这两篇。

### 8.4 一条纪律

`-cpu rv64,h=true` 依然是承重件（与 v1 相同，见
[00-design.md §5.2](00-design.md)）。默认 rv64 CPU 不暴露 H，`hyp_supported()`
会让所有调用返回 NOT_SUPPORTED，于是 gate 变成"过了但什么都没测"。

> **本条在 QEMU 10.0.13 上没能复现，已实跑核对。** `user/cmds/core/hypvm.c` 的
> 使用文档那一片跑了四组 `-cpu`（`h=true` / `rv64` / 完全不写 / `h=false`）：
> **QEMU 10.0.13 的默认 `rv64` 已经带 H**，去掉 `-cpu` 门禁照样过；而
> `-cpu rv64,h=false` **不是**得到 `NOT_SUPPORTED`，是宿主在 `hyp_arch_vcpu_exit`
> 里 KERNEL PANIC——`hyp_probe()` 只读一次 `hstatus` 看会不会陷，而 QEMU 在
> `h=false` 下这个 CSR 照样可读。逐条数据与命令见
> [03-usage.md §1.1](03-usage.md)。**建议保留该参数**（它把意图写进命令行，
> 换一台默认不带 H 的 QEMU 时不会静默退化），但别把"去掉它门禁会红"当事实。

---

## 9. 与 Linux KVM 的设计差异

| 维度 | KVM RISC-V | 本设计 |
| --- | --- | --- |
| 内存来源 | 用户态 `KVM_SET_USER_MEMORY_REGION` 登记 memslot，QEMU 侧维护 GVA→HVA 映射 | guest 帧由**内核自己**按需分配，`FRAME_F_GUEST` 出借，`mm_s2_audit` 双向交叉检查 |
| guest RAM 生命周期 | memslot 是一段被 pin 的宿主内存，QEMU 持有 | 一组帧的集合，引用计数 + 身份标志；VM 死了没还帧，审计能抓到 |
| 记账 | QEMU 进程 + cgroup（QEMU 进程作为 unit） | guest 帧记在**宿主任务自己的 cgroup** 上（`cg_mem_charge()`），VM 没有独立预算身份 |
| 用户态接口 | /dev/kvm ioctl + QEMU 的虚拟 CPU 抽象 | Native ABI 的五个调用；Linux ABI 桥上的八个调用（VM/vcpu 用 pid 打标签的槽表，v2 又加了 `set_boot` / `set_marker` / `status`）|
| 缺页处理 | QEMU 的 userfaultfd / 缺页 ioctl 回调 | 内核态 `hyp_ram_fill()`，guest 自己分配什么就补什么 |
| 设备 | QEMU 的完整设备模型（virtio、PCI、virtio-net…） | 只有 16550 与 CLINT mtime，其余 RAZ/WI |
| vCPU 调度 | vCPU 是可抢占的线程，有独立的定时器与调度类 | 一个全局单 guest 槽，`spin_trylock` 抢，输了回 `-EBUSY` |

核心差别只有一条：**KVM 把"这台机器长什么样"整体放在用户态，内核只提供
stage-2 机制；本设计把机器的形状收进内核，因为 stage-2 页表、帧标志与审计在
单级内存模型里已经是一等公民，把它们拆到用户态会立刻失去审计。** 代价是设备
模型与多 vCPU 都要在内核侧重建，这正是 [02-roadmap.md](02-roadmap.md) 里后
几项的由来。

capability 叙事的正主在 [02-roadmap.md 的 §3](02-roadmap.md)（零拷贝 VMO
guest RAM）：guest 的页不再由 hypervisor 分配，而是宿主 VMO 的一页被**借**进
stage-2。"出借"已经是 `FRAME_F_GUEST` 的语义，零拷贝只是把它从帧级推到
VMO 级。

---

## 10. 已知限制

1. **单 vcpu。** 全局槽 + trylock（`hyp_vcpu.c:1157-1190`）。guest 的 secondary
   路径要 `sbi_hart_start()`，不在 v2 SBI 面里。
2. **不注入外部中断。** —— **本条此前写的是"guest 的时钟中断与外部中断不会到来"，
   前半句是错的，已更正。** `hideleg` 委托了 VS 级中断位
   （`HYP_HIDELEG_DEFAULT` = bit 2/6/10，`kernel/include/hyp/hyp_vcpu.h:226-227`，
   写的地方 `kernel/arch/riscv64/hyp/hyp_arch.c:284`），`hstatus.VIE` 在
   `HYP_HSTATUS_GUEST_ON` 里（`hyp_arch.c:151,166-167,315`），而 vstimecmp 到期
   置 `mip.VSTIP` 是 QEMU 的硬件行为（`target/riscv/time_helper.c:25-30`
   的 `riscv_vstimer_cb()`；写 CSR 0x24d 挂上它的是 `target/riscv/csr.c:1680-1692`）。
   V=1 时 `hsie` 恒 1（`target/riscv/cpu_helper.c:585`），被委托的 VS 位被重编号
   后交给 VS-mode（`:617-634`）。**所以 guest 用 Sstc 时定时器中断是通的**，两条入口
   本树都接上了：guest 自己写 CSR 0x24d 由虚拟指令路径仿真
   （`kernel/hyp/hyp_vcpu.c:988-1003`），或 guest 发 legacy `set_timer` 由分派器代写
   （`hyp_vcpu.c:386-388`）。

   **真正缺的是外部中断注入**：`hvip`（CSR 0x646）与 `hgeip`/`hgeie` 在全树零引用
   （`grep -rn 'hvip\|hgeip\|hgeie' kernel/ user/cmds --include=*.c --include=*.h
   --include=*.S` 无输出），PLIC 没有模型（`kernel/hyp/hyp_dev.c:472-477` 把非
   UART/CLINT 页 RAZ/WI 并按页计数，`hyp_dev_note_unknown()` 在 `:100-112`），
   UART 的 IIR 恒回"无中断待处理"（`hyp_dev.c:71-76,286`）。规格 `norm:hip_vseip_vstip_acc_op` 那两条 OR 来源里，
   `vstimecmp` 那一半在位，`hgeip`/`hvip` 那一半没有。

   后果：单 vcpu、无 PLIC、无 IPI 的启动冒烟能在有 tick 的情况下跑完（前提是启动
   路径不依赖 tick），而任何依赖**外部**中断推进的 guest 行为（PLIC 配置完成后的
   外设中断、guest 之间的 IPI）现在做不了。这条是"中断虚拟化"整件事，属于
   [02-roadmap.md](02-roadmap.md) §2。**未验证**：本片没有跑过"guest 收到自己的
   定时器中断"这条路径——`timer_preempt` 之类的 guest 侧证据要等 guest 活到那一步。
3. **无 guest 页保护。** 窗口页一律 RWX。guest 之间的隔离要等 vmid 侧的真隔离
   语义才有意义，而本设计一次只跑一个 guest。
4. **GPA 上限 512 GiB。** hgatp 是 Sv39x4 但只编程根表 [0,512)（
   `kernel/include/hyp/hyp.h:26-30`）。见 [02-roadmap.md](02-roadmap.md) 的 x4
   根表项。
5. **VMID 只增不重用**（`hyp.h:26-30` 与 `hyp_arch.c:50-54`）：宽度是 WARL，
   环绕是文档化限制。
6. **设备模型是寄存器级的，不是 virtio 的。** 这直接决定了 PASS 的定义只能是"到达
   banner"而不是"引导完成"（§8.3）。**但"guest 挂不上 rootfs"这句话要分情况**：
   `RAMFS_USER=0`（默认，`Makefile:111`）镜像的根 ramfs 里没有 `/bin/init`，
   `init_kthread()` 会 `panic("init: no init program found")`（`kernel/main.c:352,361`）；
   `RAMFS_USER=1` 镜像则把 `init`、`mksh`、`ls`、`cat`、`ps` 等直接嵌进内核 ramfs
   （`Makefile:1238` 的 `RAMFS_USER_PROGRAMS`、`kernel/fs/rootfs_user.c:31-41`，
   由 `kernel/fs/diskfs/ramfs.c:1285-1301` 铺开并软链 `/bin/sh -> /bin/mksh`），
   **那种 guest 不需要块设备也能起 init**。门禁 `smoke-hyp-a20os` 的构建变量是
   `RAMFS_USER=0`（`tools/smoke_cases.py:630`），所以对门禁而言原话成立。
   **`RAMFS_USER=1` 的 guest 本轮已经构建并跑过**：`make smoke-hyp-console` 与
   `make smoke-hyp-shell` boot 的就是它（日志第 227 行
   `HYPVM: guest=/bin/boot/guest-kernel-ramfs.elf`），它确实不需要块设备就走到了
   banner，**并且 `smoke-hyp-shell` 绿了——它走到了 mksh、执行命令、`exit` 收尾**
   （§8.3 第五次）。真 rootfs（读镜像上的文件、持久写）仍然要 virtio-blk，属于
   [02-roadmap.md](02-roadmap.md)。**guest 的 virtio-blk 是内核内驱动**——
   `kernel/drvmod/examples/virtio_blk.c` 只是 `#include` 了
   `kernel/drivers/block/virtio_blk.c` 的 16 行注册壳
   （`components/drivers.toml:20-24` 把它登记成 early `.a20drv`，
   `kernel/arch/riscv64/platform/early_drivers.c:33` 从 `/boot/drivers` 加载），
   不存在"guest 的 virtio 驱动是用户态程序"这回事。
7. ~~**R|W|X 叶页依赖 QEMU 的具体行为**~~ —— **已推翻并修正，见
   [00-design.md §5.3 第 3 条](00-design.md)。** 原结论说 W 与 X 同时置位在叶 PTE
   里是保留组合、G-stage 走查直接拒；QEMU 10.0.13 的 `get_physical_address()` 只特判
   `rwx==6`（W|X）与 `rwx==2`（W），`rwx==7`（R|W|X）落到
   `PAGE_READ|PAGE_WRITE|PAGE_EXEC`，正常翻译。工作树按
   `HYP_RAM_PAGE_PROT (PTE_R|PTE_W|PTE_X)` 装 guest RAM。
   本条此前还写着"vendored 的 QEMU 树里没有 `target/` 目录，无法核对"——
   **该目录存在**（`/tmp/qs/qemu-10.0.13/target/`），核对方式是读
   `target/riscv/cpu_helper.c`。
8. **`smoke-hyp-a20os` 已经跑过，PASS。** 本条此前写的是"本文写作时尚未运行，本文
   关于 v2 行为的一切描述都是推演"。运行结论与逐行日志见
   [03-usage.md §3.3](03-usage.md)（`smoke-hyp-a20os` 与手工 `hypvm` 两次真跑，
   guest 侧结果逐字段相同）。仍然只验到 banner 一层，banner 之后的事见 §10 第 9 条。
9. **一次宿主会话里只能引导一台 guest。** 实测：同一 shell 里连续两次运行
   `/hypvm`，第一次正常 PASS，第二次——**参数与第一次完全相同**——打完
   `HYPVM: running` 之后一个 guest 控制台字节都没有，`hyp_vcpu_run()` 一直不
   返回，QEMU 100% CPU，宿主连一条 `[ERR] hyp:` 都没打（说明 guest 在跑、且没有
   陷入）。单独跑 `hypvm -m 96`、单独跑自定义 `-b` 都正常，所以与参数无关，是第
   一次 guest 以 `HYP_EXIT_FAULT` 退出之后留下的状态。根因未查（怀疑与第 2 条
   "不注入外部中断"下 guest 等待一个永远不会被投递的外部中断有关——注意不是
   时钟中断，时钟中断那条路是通的），修它属于
   [02-roadmap.md](02-roadmap.md) §2 的范围。**后果**：`smoke-hyp-vm` 与
   `smoke-hyp-vm-96` 因此是两个门禁、两次 QEMU 启动，而不是一个门禁里的两行
   命令。
---

## 11. 用户入口：hypvm

`/hyp_boot`（`user/cmds/core/hyp_boot.c`）是这套引导的**门禁**：所有参数编译期
写死，因为 `smoke-hyp-a20os` 按它打印的字面行判定（§8.3）。它不适合当工具——
想在 shell 里换一台 guest 的内存大小都做不到。

`/hypvm`（`user/cmds/core/hypvm.c`）是同一套引导的**用户入口**：ELF 路径、
RAM 窗口、guest 的 `bootargs`、marker、入口 GPA 全部来自 argv，默认值逐个等于
`hyp_boot` 的编译期常量，所以裸跑 `hypvm` 和 `hyp_boot` 引导的是同一台机器。
两者共用的机械部分（ELF 装载、最小 FDT、八个系统调用的包装）提炼在
`user/cmds/core/hyp/hyp_guest.c`；它只返回结果、不打印，所以两个程序各自的措辞
互不牵连。

**怎么用（命令行、输出行逐行含义、排查）见
[03-usage.md](03-usage.md)。** 本节只留三件属于设计记录的事。

### 11.1 FDT 放在窗口顶部，而不是固定地址

`hyp_boot` 把 DTB 写死在 `0x87f00000`，那个数只对 128 MiB 的窗口成立：窗口一小，
它就落进 guest 镜像里，而 `entry.S` 在读 `a1` 之前用自己的 `la` 指令清 bss，会把
它擦掉（§7.2、§7.4 的两条约束同时发作）。`hypvm` 因此从窗口顶端往下留 1 MiB 放
FDT（`hypvm.c:300`），并在它与镜像重叠时报错而不是硬塞。默认值下算出来的地址恰好
还是 `0x87f00000`，所以"参数化的工具"和"写死的门禁"在默认配置下引导的是同一台
机器——这正是 `smoke-hyp-vm` 要断言的东西。

`-g` 还多两条约束，因为 §7.4 那条硬约束对用户是可见的：基址必须页对齐
（`hyp_vm_load()` 整页取），且整个窗口必须落在 `entry.S` 铺的 16 GiB identity 映射
里，否则 `__boot_dtb_ptr` 在 guest 装上 `satp` 之后就不是可解引用的指针。

### 11.2 判据不变，措辞可变

`hypvm` 的 PASS 判据与 `hyp_boot` 同款且只有两条：marker 命中（设备模型只在
**guest** 控制台字节上匹配，所以宿主自己那行同名 banner 顶不上，见 §8.2），以及
`console_bytes` 大于 marker 长度（排除"只打到开头"的退化命中）。**`exit=` 与 PASS
同时出现不构成矛盾，这一点本轮又验证了一遍**：实测那次是
`exit=1(shutdown)`，而它来自 guest panic 之后的 SBI SRST ecall
（本轮第一次是 `kfree`/`[SLAB BUG]`，第四次是内核栈页 use-after-free，§8.3）。所以"`exit=2(fault)` 与 PASS 同时出现是预期的"这句话**已被实测结果取代**——
换成更强的说法：**任何 `exit=` 值与 PASS 同时出现都是预期的**，判据只看 marker。
"guest 没有块设备、挂不上 rootfs"这条边界也仍然成立，**而且它就是当前
`smoke-hyp-a20os` 的实测终点**：门禁构建的是 `RAMFS_USER=0` 镜像
（`tools/smoke_cases.py:630` 只传 `ARCH=riscv64 ABI=linux BRINGUP=0`），
guest 走到 `init_kthread` 后 `panic("init: no init program found")`（当前日志
622-625 行）。**更早几轮 guest 还没走到那里就停了**（slab 层 `kfree`、进调度器
时的栈页 use-after-free），两个根因都已修（§8.3 第五次）。块设备与
`RAMFS_USER=1` 那条替代路的完整拆解见
[03-usage.md §4.3](03-usage.md) 与 [02-roadmap.md §4](02-roadmap.md)。
`hyp_boot` 的三行字面输出被 `smoke-hyp-a20os` 按字面匹配，所以它一个字都没改；
`hypvm` 的 PASS 行带上窗口大小，正是为了让门禁能分辨两次不同参数的运行。

### 11.3 门禁是两个，不是一个

`smoke-hyp-vm` 跑一次裸 `hypvm`（默认参数），`smoke-hyp-vm-96` 跑
`hypvm -m 96 -b ...`；两条期望行都锚在各自报告的窗口大小上，否则一条
`HYPVM: PASS` 的 grep 会被任何一次运行满足，等于没测出参数有没有生效。第二个门禁
为什么必要，就是 §11.1：窗口不是 128 MiB 时固定 DTB 地址必错。

**是两次 QEMU 启动，不是一个门禁里的两行命令**：§10 第 9 条，同一会话里第二次
guest 引导不回来。把两行塞进一个门禁只会得到一个必红的门禁。

`-cpu rv64,h=true` 依然是承重件（§8.4）。没有 H 时内核对每个调用回 `ENOTSUP`，
`hypvm` 把它单独报成 `HYPVM: FAIL vm_create ...` 并以退出码 4 结束，而不是伪装成
一次引导尝试。
