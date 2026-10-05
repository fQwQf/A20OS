# hypervisor 使用指南（把 A20OS 当 guest 跑起来）

最后核实：本文写作时 `user/cmds/core/hypvm.c` 与本篇**并行开发**，本篇按任务书
给定的签名先写，随后**已按落地的实现逐行核对**（§2.2 的参数表与 §3.1 的输出表
读的是 `hypvm.c` 本身）。若集成阶段实现再变，以实现为准并回来改本文。

标注约定沿用 [01-a20os-guest.md](01-a20os-guest.md)：**已验证** = 本片读过代码
或跑过命令并看到了输出；**未验证** = 设计推演或依赖尚未落地的实现，本片没有
跑过。

本篇是**操作**文档。四篇的分工：

| 篇 | 讲什么 | 什么时候读它 |
| --- | --- | --- |
| [00-design.md](00-design.md) | 地基设计：stage-2 怎么复用单级内存模型、riscv64 架构半、vcpu 运行循环 | 你要改内核侧 |
| [01-a20os-guest.md](01-a20os-guest.md) | v2 设计：委托表、VS CSR、SBI 面、二级缺页路由、设备模型、引导协议、冒烟判据 | 你要改设备模型或引导 |
| [02-roadmap.md](02-roadmap.md) | v2 之后的切片顺序与前置条件 | 你要问"为什么还没有 rootfs" |
| **本文** | **怎么用**：前置条件、命令行、输出解读、限制、排查 | **你要跑一次 guest** |

---

## 1. 前置条件

### 1.1 目标必须是 riscv64，且 CPU 必须带 H 扩展

只有 riscv64 走这条 hypervisor 路径。其它架构的 `hyp_supported()` 恒为 0，
所有 hyp 系统调用直接回 `-EOPNOTSUPP`（值 95，`kernel/include/core/errno.h:43`）。
非 riscv64 架构编译出来的镜像里 `hyp_boot`/`hypvm` 也在，但它们一调用就失败。

**QEMU 应该显式给 `-cpu rv64,h=true`——但树里对它的说法有一处与实测不符，先说
清楚，免得你按错误的心智模型排查。**

树里的说法（`00-design.md` §4/§5.2、`01-a20os-guest.md` §8.4、`smoke_cases.py`
的注释）是：**默认 `rv64` CPU 不暴露 H，`hyp_supported()` 因此返回 0，每个 hyp
系统调用回 `-EOPNOTSUPP`。**

**本片实测（QEMU 10.0.13，Debian 1:10.0.13+ds-0+deb13u1）：复现不出来。** 三次
运行，同一个镜像与内核，只有 `-cpu` 不同：

| `-cpu` | `hyp_supported()` | 结果 |
| --- | --- | --- |
| `-cpu rv64,h=true` | 1 | `HYPVM: PASS marker_seen=1 console_bytes=163 exit=2(fault) mem=128 MiB` |
| `-cpu rv64`（即默认） | 1 | **同样 PASS**，输出逐字节相同 |
| 完全不写 `-cpu` | 1 | 同样 PASS |
| `-cpu rv64,h=false` | **仍然是 1** | **宿主 KERNEL PANIC**（不是 `-EOPNOTSUPP`） |

两条结论：

1. **这台 QEMU 的默认 riscv64 CPU 已经带 H**，所以门禁里那个
   `-cpu rv64,h=true` 在 QEMU 10.0.13 上**不是承重件**——去掉它门禁照样过。
   本片是拿三次真跑得出的，不是读源码推的。
2. **`-cpu rv64,h=false` 不会得到 `-EOPNOTSUPP`。** `hyp_probe()`
   （`kernel/arch/riscv64/hyp/hyp_arch.c:24-35`）只做一件事：读一次 `hstatus`
   看会不会陷。QEMU 在 `h=false` 下这个 CSR 仍然可读，于是 `hyp_supported()`
   返回 1，`vm_create` 成功，工具一路走到 `HYPVM: running`，然后宿主死在
   `hyp_arch_vcpu_exit` 里：

   ```
   [ERR] Kernel Illegal Instruction at sepc=0xffffffc0804409f4
   ========== KERNEL PANIC ==========
   [PANIC] task: pid=6 name=hypvm state=2
   [PANIC] backtrace:
     [0] kernel_trap_handler+0x274
     [1] hyp_arch_vcpu_exit+0x4e0
   ```

   也就是说：**"CPU 没有 H" 这条故障在 QEMU 上表现为宿主 panic，不是干净的
   `EOPNOTSUPP`。** 探测手段（读一个在关掉 H 时依然存在的 CSR）分辨不了这两种
   情况——这是探测方法本身的局限，见 §5.1 对应行。

**那还要不要写 `-cpu rv64,h=true`？要写。** 它把意图写进命令行，换一台默认 CPU
不带 H 的 QEMU 时不会静默退化成"什么都没测"。只是别把"去掉它门禁会红"当成
事实——在 QEMU 10.0.13 上它去掉也过。**已验证**：`tools/smoke_cases.py:553` 起的
case 定义里 `-cpu rv64,h=true` 是硬写进 argv 的。

```sh
# 交互式跑 QEMU 时自己带
qemu-system-riscv64 -machine virt -cpu rv64,h=true ...
```

### 1.2 宿主内核要含 hyp/ 目录

`kernel/hyp/`（`hyp.c` / `hyp_vcpu.c` / `hyp_dev.c`）与
`kernel/arch/riscv64/hyp/` 是架构无关半与 riscv64 架构半。**已验证**：主线
`feat/virt-foundation` 已含这两半（提交 `d7a1e0268`、`9858550b8`、`6364d1138`、
`1aa0603ea`、`1214ea6d5`、`9e2f465bf`）。

### 1.3 guest 是什么

**guest 是同一棵树构建出来的 riscv64 A20OS 内核 ELF**，不是另一个项目、不是
QEMU 自带的 OpenSBI payload。它是宿主内核自己，只是被装载进 stage-2 而不是被
CPU 直接跑。

**已验证**（`tools/targets-images.mk:6-10`）：镜像构建时把
`$(BUILD_DIR)/kernel-nosyms.elf` 拷到 FAT32 镜像的 `::/boot/guest-kernel.elf`。
`/bin` 是 FAT32 镜像在运行系统里的挂载点（`kernel/fs/mount_setup.c` 的
`mount_block_devices()` 选 `utilities_path = "/bin"`，`hyp_boot.c:36-42` 的注释
同此），所以运行系统里的路径是 **`/bin/boot/guest-kernel.elf`**。

只有 riscv64 会拷（`GUEST_KERNEL_DEP` 在非 riscv64 下为空），别的架构镜像里没有
这个文件。

因为是同一个内核，**宿主和 guest 会打出逐字节相同的一行**
（`    A20OS Kernel `）。这是 marker 机制存在的全部理由，见 §3.2。

---

## 2. 快速开始

### 2.1 路线 A：门禁（自动化）

```sh
make smoke-hyp-a20os
```

**已验证**：`tools/targets-smoke.mk:170-171` 就是这一行，转发给
`tools/smoke.py smoke-hyp-a20os`。这条门禁自己会：

1. 用 `ARCH=riscv64 ABI=linux BRINGUP=0` 做 `dev-build`（`smoke_cases.py:556`）；
2. 把 `kernel-nosyms.elf` 放进镜像的 `/boot/guest-kernel.elf`；
3. 用 `-cpu rv64,h=true` 起 QEMU，等 shell 提示符 `# `；
4. 敲 `hyp_boot` 再敲 `poweroff`（`:558-560`）；
5. 断言 console 出现 `HYP_A20OS: PASS`，且**不**出现 `LOCK-STALL` /
   `MCS DEADLOCK` / `HYP_A20OS: FAIL`（`:573-575`）；
6. 日志落在 `.kernel-build/smoke/hyp-a20os-riscv64.log`。

timeout 是 **300 s** 而不是别的 case 常用的 60 s（`:562-565`）：guest 的每一个
console 字节、每一次页表走查、每一次二级缺页都要陷回宿主，guest 引导比宿主自举
慢好几个数量级。

**门禁的 `forbid` 刻意不含 `PANIC`**（`:566-572` 的注释）：guest 没有块设备，
挂不上 rootfs，会在 `init_kthread` 里 panic（"init: no init program found"），
而 guest 的 panic 文本与宿主的逐字节相同——把 `PANIC` 放进 `forbid` 会让一次
正确的运行判红。宿主真的死掉由"缺了 PASS 期望"抓住。

### 2.2 路线 B：命令行（手动）

工具名 `hypvm`，源码 `user/cmds/core/hypvm.c`（**已验证**：本片读过该文件）。
它把 `hyp_boot.c` 的全部编译期常量变成命令行参数，其余逻辑一字未改。`-h` /
`--help` 打印用法与退出码表。

```
hypvm [-k <guest-kernel.elf>] [-m MiB] [-g gpa_base] [-b bootargs]
      [--marker <str>] [-e guest_entry_gpa] [-h]
```

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `-k <path>` | `/bin/boot/guest-kernel.elf` | guest ELF 路径（运行系统里的路径，不是宿主开发树的路径） |
| `-m <MiB>` | `128` | guest RAM 窗口长度，MiB。窗口是 `[base, base + m)`。**下限 2 MiB**（`hypvm.c:206`） |
| `-g <gpa>` | `0x80000000` | 窗口基址，必须页对齐，且整个窗口必须落在 `[0x80000000, +16 GiB)` 里（`hypvm.c:212-227`） |
| `-b <str>` | `a20.hypguest=1` | 写进 DTB 的 `/chosen/bootargs`（`hypvm.c:50`） |
| `--marker <str>` | `A20OS Kernel` | guest console marker，长度必须 1..32 字节（`hypvm.c:63,198`；上限是 `hyp_vcpu.h` 的 `HYP_VM_MARKER_MAX`，超长在内核里被静默截断，那会让"报告的 marker"和"实际匹配的 marker"变成两个串） |
| `-e <gpa>` | 取自 ELF | guest 入口 GPA。必须在 RAM 窗口内（`hypvm.c:278-283`） |
| `-h` / `--help` | — | 打印用法与退出码表，退出 0 |

**入口默认"取自 ELF"具体是什么**：`e_entry` 是**虚拟地址**（内核链在高半区，
`VIRT_BASE = 0xFFFFFFC080200000`，`kernel/arch/riscv64/boot/ldscript.ld:18`），
不是 guest 物理地址。装载器按 program header 把它换算成 LMA：`entry_gpa =
ph.p_paddr + (e_entry - ph.p_vaddr)`（`hyp_guest_load_elf()`，
`hyp_guest.c:203-205` 是同一段逻辑）。**已验证**：对本树的 `kernel-nosyms.elf` 跑
`readelf -h`，`e_entry = 0xffffffc080200000`；对应的 `PT_LOAD` 段
`PhysAddr = 0x0000000080200000`，所以入口 GPA 是 `0x80200000`，与 `platform.h:20`
的 `KERNEL_ENTRY` 一致，也与冒烟日志里的 `entry_gpa=0x80200000` 一致。

**退出码**（`hypvm.c:70-75`，是工具接口的一部分，脚本靠它区分失败类别）：

| 码 | 含义 |
| --- | --- |
| 0 | PASS |
| 1 | guest 跑了，但 marker 判据没过 |
| 2 | 参数错 |
| 3 | guest ELF 读不到 |
| 4 | 内核拒绝了 hyp 系统调用。**注意本片没能复现出这一类**：见 §1.1 的实测表——默认 `rv64` 与 `rv64,h=true` 行为相同，而 `rv64,h=false` 走的是宿主 panic 而不是 `EOPNOTSUPP` |
| 5 | 镜像/入口/DTB 装不进窗口 |

跑法与门禁里一样——进 shell 之后敲：

```
# hypvm
# hypvm -m 96 -b 'a20.hypguest=1,a20.hypvm=manual'
```

> **`-g` 实际上只有默认值是安全的。** 镜像的段 LMA 由链接脚本钉死在
> `0x80200000` 一带（`ldscript.ld:17` 的 `PHYS_BASE`），装载器照 LMA 装，**不会**
> 跟着 `-g` 搬。`-g` 挪走的只是**按需填充的 RAM 窗口**，于是窗口与镜像分家。
> **已验证**：`hypvm -g 0x90000000 -m 64` 能过所有前置检查（该区间确实落在
> `[0x80000000, +16 GiB)` 内），回执打出
> `base=0x90000000 … dtb_gpa=0x93f00000`，紧接着 `entry_gpa=0x80200000`
> ——**入口落在窗口外**，然后就停在 `HYPVM: running` 再也没有下一行：没有横幅、
> 连一条 `[ERR] hyp: trap #N` 都没有，直到 200 s 超时。`image_end > base + mem`
> 这条检查抓不住它，因为 `0x810E0914 < 0x94000000`（`hypvm.c:288-295`）。
>
> 结论：**`-g` 是给"未来某个链接到别处的 guest"留的口子，不是给现在这个 guest
> 用的旋钮。** 现在只改 `-m`。

### 2.3 DTB 放在哪

DTB 由工具合成，不从文件读。**放置规则**（**已验证**，`hypvm.c:53-58,296-317`）：

> DTB 放在**窗口顶端往下 1 MiB** 处，向下对齐到页。
> `dtb_gpa = (base + mem - 1 MiB) & ~0xFFF`

留出的 1 MiB 是余量而不是需求（blob 本身 < 2 KiB），买的是"bootargs 变长时
地址不动"。这个规则要满足三条约束，每条都对应 `hyp_boot.c` 注释里的一个坑：

1. **必须在镜像之外。** `entry.S` 在读 a1 之前用 `la` 指令自己清 BSS；DTB 停在
   镜像内部会被那串 store 抹掉（`hyp_boot.c:51-56`）。`hypvm` 显式检查
   `dtb_gpa < image_end` 并拒绝（`hypvm.c:312-317`）。
2. **必须在 guest 的启动 identity 映射窗口内。** `entry.S` 把
   `[BOOT_MAP_PHYS, +16 GiB)` identity 映射（十六个 1 GiB 槽，`entry.S:94-155`，
   `BOOT_MAP_PHYS = 0x80000000`）。a1 按 SBI 约定是**物理地址**，而
   `__boot_dtb_ptr` 被 `riscv64_memory_init()` 与 `fdt_extract_bootargs()`
   **当普通指针解引用**——那是在 guest 装上自己的 satp **之后**。DTB 落在启动
   identity 窗口之外，guest 在打印任何东西之前就缺页（`hyp_boot.c:57-63`）。
   `hypvm` 因此在**跑之前**就拒绝越界的 `-g`（`hypvm.c:220-227`，退出码 5）——
   这条检查放在前面是因为它是"能打印"与"第一次读 DTB 就缺页"的分界，而从 guest
   侧看不见。
3. **必须在 RAM 窗口之内**，否则 `hyp_vm_load()` 装不进去（`hypvm.c:304-310`）。

对比：`hyp_boot` 把 DTB 硬编码在 `0x87f00000`（`hyp_boot.c:65`）；`hypvm` 改成像
"从窗口顶端量下来"，这样改 `-m` 或 `-g` 时 DTB 会自动跟着走。默认参数下算出来
是 `0x87F00000`，与硬编码值一致。

最小 DTB 里**必须**有的东西（`hyp_guest.c:285-316` 的注释，逐个对着 guest 侧
解析器读的；`hyp_guest_fdt_build()` 照此构造）：

| 节点/属性 | 谁读 | 缺了会怎样 |
| --- | --- | --- |
| 合法 FDT 头（version 17） | `riscv64_memory_init()` | 打印告警后回退到板级窗口 |
| `/memory@<base>/reg` | `riscv64_memory_init()` | 回退到板级窗口 `0x80000000 .. +16 GiB`，与我们要的窗口恰好一致，所以这条实际可省 |
| `/cpus/timebase-frequency` = 10000000 | `riscv64_fdt_timebase_freq()` | 回退到 arch 常量（qemu-virt 上也是 10 MHz），只是把回退值钉死 |
| `/cpus/riscv,isa` 含 `sstc` | `riscv64_fdt_has_isa_extension()` | **致命**：guest 改走 SBI `set_timer`，而 v2 的 SBI 面没有它，guest 死在 `[INIT] Timer initialized`（`01-a20os-guest.md` §4） |
| `/chosen/bootargs` | `arch_bootargs_get()` | 命令行为 NULL，`-b` 白给 |

isa 串固定写 `rv64imafdch_sstc`（`hyp_guest.c:411`），**不受 `-b` 影响**——这是
承重件，不是可配项。

> **`-b` 现在是"写进去了"而不是"起作用了"。** 属性确实被合成了（`hypvm` 的回执
> 行原样回显它，`smoke-hyp-vm-96` 就是靠这一行断言它没被静默丢掉），但当前切片里
> guest 在 banner 之后就于 PLIC fault 退出（§3.3），**根本没走到读它的那一步**——
> 两次真跑（`smoke-hyp-vm`、`smoke-hyp-vm-96`）里 `HYPVM: running` 之后的日志都
> 只有 guest 的横幅、`Initializing system...` 和 UBSAN 自检，**没有一行 `[FDT]`**，
> 而 `arch_bootargs_get()` 每次都会先打 `[FDT] dtb_ptr=0x…`（`fdt.c:309`）。所以
> `-b` 的当前可观测面只有"宿主把它放进 DTB 了"，它对 guest 行为的影响要等 guest
> 能活过 `trap_init()`（§4.3）之后才谈得上。

### 2.4 装进去的三样东西

理解输出需要知道装载器做了什么（**已验证**，`hyp_guest.c:163-280`）：

- **按 `PT_LOAD` 段逐段装到各段的 LMA**，每 1 MiB 一次
  `hyp_vm_load()`（桥的单次上限 16 MiB，更小的块让宿主暂存代价与实际装载量
  成正比，`:208-211`）。
- **每段的 `[p_filesz, p_memsz)` 零填尾部单独装**，一页一次（`:243-265`）。
  这段是 `.bss`，必须**在这里**映射而不是留给按需填充，两个理由都在注释里：
  `entry.S` 第一条指令就是往 `_stack_end`（在 `.bss` 里）设 sp 然后清 bss，
  尾部没映射的话 guest 的第一条 store 就缺页——而且是约 2800 页、每页一次 trap，
  在 guest 打印任何东西之前；A20OS 在映射之前就把 sp 设进了那段 `.bss`，所以
  "按需填一页零给它"也不对：guest 的栈会是按需填出来的那页而不是镜像自己的。
- **DTB 单独一次 `hyp_vm_load()`**（`:553-554`）。

装完会检查两件事并各自拒绝：镜像末尾是否越过 RAM 窗口末尾（`:537-544`）、
DTB GPA 是否压在镜像上（`:545-551`）。

---

## 3. 输出解读

### 3.1 每一行

`hypvm` 的用户可见输出全部用 `HYPVM:` 前缀。逐行含义（**已验证**：读
`user/cmds/core/hypvm.c` 的 `printf` 逐条对过）：

| 行 | 出处 | 含义 |
| --- | --- | --- |
| `HYPVM: guest=<path> elf_bytes=<n> mem=<n> MiB base=0x… marker='<str>' bootargs='<str>'` | `hypvm.c:251` | 参数与输入的**回执**：读到了 ELF，回显实际生效的窗口、marker、bootargs。这一行里的一切都是你以为你传了的——参数写错时先看它 |
| `HYPVM: segments=<n> entry_gpa=0x… image_end=0x… dtb_gpa=0x…` | `hypvm.c:351` | ELF 解析结果：`n` 个 `PT_LOAD` 段、入口 GPA、镜像最后一段的 `p_paddr + p_memsz`、DTB 落点 |
| `HYPVM: running` | `hypvm.c:354` | 即将 `hyp_vcpu_run()`。这之后到下一行之间，**guest 的输出与宿主的输出在同一行控制台上交错** |
| `HYPVM: exit=<n>(<name>) scause=0x… stval=0x… htval=0x… marker_seen=<n> console_bytes=<n>` | `hypvm.c:372` | 运行结束。`<name>` 是 `none`/`shutdown`/`fault`/`error`（`hyp_vcpu.h:53-58`）；`scause`/`stval` 是 guest 的寄存器值；`htval` 是二级缺页的 GPA **右移 2 位**的原始值（`hyp_vcpu.h:100`），**真实 GPA 是 `htval << 2`** |
| `HYPVM: PASS marker_seen=<n> console_bytes=<n> exit=<n>(<name>) mem=<n> MiB` | `hypvm.c:391` | 判据通过。四个数一并打出来，让人不用回头翻上一行 |
| `HYPVM: FAIL <what> (rc=… errno=…)[: <说明>]` | `hypvm.c:111,119` | 失败。`<what>` 是失败环节；带 `: <说明>` 的那批来自 `fail_call()`，额外说明这个 errno 是不是"内核不支持" |
| `HYPVM: FAIL <具体消息>` | 各处 | 参数/布局类的具体拒绝理由，逐条见 §5.1 |

**注意 `HYPVM: FAIL` 的退出码不都是 1。** 见 §2.2 的退出码表：`vm_create` 之类
的系统调用被内核拒绝（多半是没有 H 扩展）回的是 **4**，不是 1。脚本里判"guest
跑了但没到"要看 1，判"这台机器根本不支持"要看 4。

宿主内核侧还会打一些 `[ERR] hyp: …` / `hyp: …` 行，它们**不是** `hypvm` 的
输出，但排查时要看（见 §5.1 与 §5.2）。

**一次成功运行的完整输出**（**已验证**，本片在 QEMU 里真敲 `hypvm` 得到的，
中间省略了 guest 自己那 40 多行启动日志与宿主的 trap 轨迹）：

```
HYPVM: guest=/bin/boot/guest-kernel.elf elf_bytes=4618448 mem=128 MiB base=0x80000000 marker='A20OS Kernel' bootargs='a20.hypguest=1'
HYPVM: segments=3 entry_gpa=0x80200000 image_end=0x810e0914 dtb_gpa=0x87f00000
HYPVM: running
    A20OS Kernel                 ← guest 自己的横幅，宿主也打过一模一样的一行
Initializing system...
[ERR] hyp: undecodable guest access at pc=ffffffc080445ada (second-stage fault)
HYPVM: exit=2(fault) scause=0x17 stval=0xc000000 htval=0x300000a marker_seen=1 console_bytes=163
HYPVM: PASS marker_seen=1 console_bytes=163 exit=2(fault) mem=128 MiB
```

`elf_bytes=4618448` 是本树 `kernel-nosyms.elf` 的大小，`dtb_gpa=0x87f00000`
正是 §2.3 那条"顶端下量 1 MiB"算出来的值（`0x88000000 - 0x100000`）——**恰好等于**
`hyp_boot` 硬编码的 `GUEST_DTB_GPA`（`hyp_boot.c:87`），所以两个工具在默认参数下
把 DTB 放在同一个地址。

### 3.2 PASS 判据是 marker，不是"guest 没退出"

`PASS` 的定义是 **guest 到达了自己的横幅**，判据有两条（**已验证**，
`hypvm.c:385-389`）：

1. `marker_seen != 0`；
2. `console_bytes > strlen(marker)`——排除"只匹配到开头就停"的退化命中。

**为什么必须是 marker 而不是日志比对。** 宿主和 guest 是同一个内核，共享一根
16550，两边的日志行会交错。v1 的门禁靠两行 console 输出的交叉比对
（`^HYP$` 与 `HYP_VCPU_TEST: PASS`，`smoke_cases.py:533`），那在 guest 只打三个
字符时可行；v2 的 guest 会打整个内核启动日志，`[FDT]` 这类前缀在两边都可能出现，
这种判据在日志变长之后就不再是判据（`01-a20os-guest.md` §8.1）。

marker 只在**guest UART 字节**上比对（设备模型的 `hyp_dev_console_byte()`，
`kernel/hyp/hyp_dev.c`），所以宿主自己打的那行逐字节相同的
`    A20OS Kernel ` 不会让它命中。命中一次之后立刻停匹配
（`hyp_dev_console_byte()` 里 `marker_seen` 的早退分支）。

`--marker` 选什么串，等于决定这条判据证明多少事情。强度等于它所证明的事情的
强度：打在启动早期的短串只证明"进了 C 代码并能写串口"；打在内存子系统就绪
之后的串才顺带证明按需 RAM 窗口工作。默认值 `A20OS Kernel` 是 guest 最早的
banner（`kernel/main.c` 在 `kernel_main` 的第一件事就打印它，早于 `uart_init`），
保守但够用。

### 3.3 guest 在横幅之后 fault 退出——这是设计内行为

**已验证**（本片两次真跑，见下）：

**(a) 门禁那条**——`python3 tools/smoke.py smoke-hyp-a20os`，退出 0、
`smoke-hyp-a20os: PASS`，日志 `.kernel-build/smoke/hyp-a20os-riscv64.log`
第 261-263 行：

```
[ERR] hyp: undecodable guest access at pc=ffffffc080445ada (second-stage fault)
HYP_A20OS: exit=2(fault) scause=0x17 stval=0xc000000 htval=0x300000a marker_seen=1 console_bytes=163
HYP_A20OS: PASS
```

**(b) CLI 那条**——在 QEMU 里手工敲 `hypvm`（同一条 argv、同样带
`-cpu rv64,h=true`）：

```
HYPVM: guest=/bin/boot/guest-kernel.elf elf_bytes=4618448 mem=128 MiB base=0x80000000 marker='A20OS Kernel' bootargs='a20.hypguest=1'
HYPVM: segments=3 entry_gpa=0x80200000 image_end=0x810e0914 dtb_gpa=0x87f00000
HYPVM: running
    A20OS Kernel
Initializing system...
[ERR] hyp: undecodable guest access at pc=ffffffc080445ada (second-stage fault)
HYPVM: exit=2(fault) scause=0x17 stval=0xc000000 htval=0x300000a marker_seen=1 console_bytes=163
HYPVM: PASS marker_seen=1 console_bytes=163 exit=2(fault) mem=128 MiB
```

两条的 guest 侧结果逐字段相同。**`exit=2(fault)` 与 `PASS` 同时出现**，而且这不是
一次侥幸——这是当前切片的稳定终点。

`scause=0x17` = 23 = guest store page fault（`HYP_SCAUSE_GPF_STORE`，
`hyp_vcpu.c:74`）。`htval << 2 = 0xC000028`。**已验证**这落在 PLIC 里：
`platform.h:46` 的 `PLIC_BASE = 0x0C000000`，`+0x28` 是
`PLIC_PRIORITY + UART0_IRQ * 4`（`platform.h:50,55`，`UART0_IRQ = 10`）——
即 guest 的 `trap_init()` 在做的事（`kernel/arch/riscv64/trap/irqchip.c:78`）。
反汇编 `.kernel-build/riscv64-qemu-virt-riscv64-linux-dev/kernel-nosyms.elf` 的
`0xffffffc080445ada`，那条指令是 16 位压缩指令 `c.sw a4,40(a5)`
（机器码 `0xd798`），而 `hyp_decode_guest_access()` 只解 32 位编码
（`(insn & 3) != 3` 直接返回 0，`hyp_vcpu.c:575`），于是报 "undecodable guest
access" 并退出。

**为什么这是设计内行为而不是缺陷**：guest 无块设备、挂不上 rootfs，本来就
活不到 shell（`02-roadmap.md` §4）。guest 在 banner 之后于 PLIC 区域 fault
退出，是当前切片的天花板——设备模型对 PLIC 的定位是 RAZ/WI 加每页计数器
（`01-a20os-guest.md` §6.3，`kernel/hyp/hyp_dev.c` 的
`hyp_dev_note_unknown()`），**但压缩指令的存取解码还缺**，所以连 RAZ/WI 那条
路都没走到就被判成 undecodable。这条限制只影响"guest 能走多远"，不影响 PASS
判据——`marker_seen=1` 意味着 guest 已经从入口一路走到了打印 banner。

换句话说：**`exit=2(fault)` 与 `PASS` 同时出现是正常的**，两者讲的是两件事。
看到 `exit=2` 就判失败，是把"guest 停在哪儿"误当成"guest 有没有到达"。

---

## 4. 当前限制与红线

### 4.1 契约不改

`kernel/include/hyp/hyp_vcpu.h` 是**冻结契约**（文件头逐字说明了：里面的布局是
承重件，改字段名或顺序是跨三个工作包的 ABI 断裂，不是重构）。本文只描述怎么用
它，不建议改它。发现它有问题，走一次显式的契约变更，不要各包自己分叉。

### 4.2 单 guest、单 vcpu、单 hart

- **全局只有一个 guest 槽**。`hyp_vcpu_run()` 用
  `spin_trylock_irqsave()` 抢，抢不到就回 `-EBUSY`（`hyp_vcpu.c:998-1007`），
  **不等**。两个 `hypvm` 同时跑，第二个立刻拿到 EBUSY 而不是排队。
- **单 vcpu、单 hart，hartid 只能是 0**。`hyp_vcpu_set_boot(vcpu, 0, dtb)`
  不是默认值而是 guest 入口代码需要的值：`entry.S` 在 `a0 == 0` 时直接跳过 SBI
  HSM 的 lottery 交接（`entry.S:44`，`hypvm.c:335-338`）。`a0 != 0` 会走
  secondary 分支，那里要 `sbi_hart_start()` 与 `riscv64_smp_release`，两者都不
  在 v2 的 SBI 面里。
- **多 vcpu 与中断虚拟化**在 [02-roadmap.md](02-roadmap.md) §2，未落地。

### 4.3 无 guest virtio、无 rootfs

设备模型是**寄存器级**的 16550 UART + CLINT mtime，其余 RAZ/WI。没有
virtio-blk，guest 挂不上 rootfs，会死在 `init_kthread`（"init: no init program
found"，`kernel/main.c`）。真 rootfs 见 `02-roadmap.md` §4。

**所以本文没有任何"进 guest shell"的步骤——现在进不去。** 到那一步需要
`02-roadmap.md` §3（零拷贝 VMO guest RAM）与 §4（virtio 设备模型）。

### 4.4 GPA < 512 GiB

`hgatp` 是 Sv39x4，但实现只编程根表 `[0, 512)` 的条目（`kernel/include/hyp/hyp.h:26-30`，
`kernel/hyp/hyp.c:20` 的 `HYP_GPA_LIMIT = 1ULL << 39`）。`-g` 超过这个界限的
窗口会被内核拒绝而不是静默失效（`hyp_vm_create` 与 `hyp_vm_set_ram` 都检查，
`hyp.c:90,366`）。解除需要 2048 条目的 x4 根表 + 更宽的根元数据块 +
非叶 PTE 的分段 PPN 编码，是 `pt_meta_t` 的**格式变更**，`02-roadmap.md` §5
按纪律刻意缓行。

### 4.5 宿主 IRQ 打在 guest 上会即时返回

`hideleg` 委托了 VS 级中断位，宿主中断在 guest 执行期间照样陷到 HS、走宿主
自己的 IRQ 机器，然后 `sret` 直接回 guest（`hyp_vcpu.c:893-896`：
`if (scause & HYP_SCAUSE_INTR_BIT) return 1;`）。这是**设计如此**：guest 运行
期间的宿主 IRQ 不需要为 guest 保存任何东西。代价是反过来——**HS 侧没有任何东西
去断言 guest 的中断**，所以 guest 的时钟中断与外部中断不会到来
（`01-a20os-guest.md` §10.2、`02-roadmap.md` §2.2）。任何依赖调度推进的 guest
行为现在都做不了。

### 4.6 VMID 只增不重用

每次建 VM 拿一个新 VMID 并做一次 `hfence.gvma`（`hyp.c:117`，
`hyp_vcpu.h` 的 `hyp_arch_vmid_fenced()`）。宽度是 WARL，环绕是文档化限制
（`00-design.md` §2）。反复建 VM 会持续消耗 VMID 空间。

---

## 5. 排查

### 5.1 表

| 症状 | 原因 | 怎么办 |
| --- | --- | --- |
| `HYPVM: FAIL vm_create (rc=-95 errno=95)`，**退出码 4** | 内核的 `hyp_supported()` 返回 0，桥在 `sys_a20_bridge.c:285,375,404` 三处各挡一次，返回 `-EOPNOTSUPP`（95，`kernel/include/core/errno.h:43`）。`fail_call()` 会在后面追加一句说明（`hypvm.c:108-115`） | **本片没有复现出这一类**（见 §1.1：QEMU 10.0.13 的默认 `rv64` 本来就带 H）。真遇到时按"这台 QEMU 的 CPU 不带 H"处理，加 `-cpu rv64,h=true` |
| `[PANIC] task: … name=hypvm` + `KERNEL Illegal Instruction`，backtrace 里有 `hyp_arch_vcpu_exit` | **这才是"CPU 不带 H"在 QEMU 上的样子**，不是 `EOPNOTSUPP`。`hyp_probe()`（`hyp_arch.c:24-35`）只读一次 `hstatus` 看会不会陷，而 QEMU 在 `h=false` 下这个 CSR 照样可读，于是探测放行、运行走到进 guest 那一步才炸。**已验证**：`-cpu rv64,h=false` 跑 `hypvm` 得到这个 panic（输出见 §1.1） | 加 `-cpu rv64,h=true`。这是探测方法的局限，不是新 bug |
| `hypvm` 卡在 `HYPVM: running` 之后再无输出，连 `[ERR] hyp: trap #N` 都没有 | 多半是 `-g` 挪走了 RAM 窗口而镜像仍在它的链接地址上，见 §2.2 的提示框。**已验证**（`-g 0x90000000 -m 64`，200 s 超时前零输出） | 去掉 `-g`，用默认值 |
| `HYPVM: FAIL cannot read /bin/boot/guest-kernel.elf (errno=2)`，退出码 3 | 镜像里没有这个文件 | 只有 riscv64 会拷（`tools/targets-images.mk:8-10`）；非 riscv64 镜像本来就没有。用 `-k` 指别的路径 |
| `HYPVM: FAIL guest ELF rejected (entry=0x…)`，退出码 5 | 文件不是 ELF64/LSB/`ET_EXEC`/`EM_RISCV`，或 `e_phnum` 为 0 或 > 16，或某段的 `p_offset + p_filesz` 越界，或某段 LMA 未按页对齐却要与前一段共享首页（检查项见 `hyp_guest.c:163-215`，`hyp_guest_load_elf()` 是同一份逻辑） | 确认 `-k` 指的是 riscv64 的 `kernel.elf` / `kernel-nosyms.elf`，不是别的架构的产物 |
| `HYPVM: FAIL image end 0x… past RAM window end 0x…`，退出码 5 | 镜像比 `-m` 给的窗口还大 | 加大 `-m`。已验证：本树镜像 `image_end ≈ 0x810E0914`，128 MiB 够（`0x80000000 + 128 MiB = 0x88000000`） |
| `HYPVM: FAIL dtb gpa 0x… overlaps the image (ends 0x…)`，退出码 5 | 窗口太小，从顶端下量 1 MiB 的 DTB 落点压进了镜像 | 加大 `-m`（或把 `-g` 抬高）。DTB 落进镜像范围会被 `entry.S` 清 BSS 的 `la` store 抹掉 |
| `HYPVM: FAIL -g 0x… + N MiB is outside the identity map entry.S lays down`，退出码 5 | 窗口越出了 `[0x80000000, +16 GiB)` | 换 `-g` / `-m`。这是**跑之前**的检查（`hypvm.c:220-227`），因为越界的症状是 guest 第一次读 DTB 就缺页，而那从 guest 侧看不见 |
| `HYPVM: FAIL -m N is below the 2 MiB floor` / `-g 0x… is not page aligned` / `marker must be 1..32 bytes` / `unknown argument '…'`，退出码 2 | 参数错 | 按 §2.2 的表改。`-h` 打印用法 |
| `HYPVM: FAIL guest never printed '<marker>' (exit=N)`，退出码 1 | guest 跑了但 marker 没命中 | 两条可能：`--marker` 与 guest 实际横幅不一致；或 guest 卡在它自己的早期初始化。**没命中说明 guest 连一行 banner 都没打完**，与 §3.3 那种"打完横幅才停"是不同的问题 |
| `HYPVM: FAIL guest wrote N bytes, marker is M: hit is degenerate`，退出码 1 | 退化命中：guest 只写了 marker 本身那么几个字节 | 极少见。guest 只打印了横幅就死，且刚好停在 marker 末尾 |
| `running` 之后**过一会儿**才出 `exit=` 行 | guest 极慢是**正常**的——每个 console 字节、每次页表走查、每次二级缺页都要陷回宿主（`smoke_cases.py:562-565` 就是为此把 timeout 提到 300 s）。**已验证**：本片的 `hypvm` 两次运行都在这个量级上正常收尾 | 等。门禁的 300 s 是按"guest 比宿主自举慢好几个数量级"定的 |
| 看到 `exit=2(fault)` 就以为失败 | **这是设计内行为**，见 §3.3 | 只看 `marker_seen` 与 `HYPVM: PASS`。`exit=2` 只说明 guest 停在哪儿 |
| `[ERR] hyp: undecodable guest access at pc=…` | 二级缺页上的访存指令解不出来（当前是 16 位压缩 load/store，解码器只认 32 位编码，`hyp_vcpu.c:575`） | 已知限制，见 §3.3。不影响 PASS 判据 |
| `[ERR] hyp: trap #N scause=… pc=… stval=… htval=…` | 宿主侧的 trap 轨迹。开头若干条 `scause=15/16/17` 是启动头几页按需填充时的正常回访 | 正常诊断输出。真正决定 guest 停在哪的是最后那条 |
| `hyp: guest MMIO at 0x… xN times (no model)` | guest 在探测没有模型的设备区，每 4096 次打一行（`hyp_dev.c` 的 `HYP_RAZ_LOG_EVERY`） | 正常诊断输出，不是错误 |
| `hyp: trap loop at pc=… scause=… (N traps so far)` | 同一处反复陷（`hyp_vcpu.c:921-927`） | 与设备模型表对照，一般是某个自旋等一个永远不来的值。契约里最该担心的一个值是 16550 的 LSR bit `0x20`：它必须常置，否则 `arch_uart_putc()` 的自旋把 guest 焊死（`01-a20os-guest.md` §6.1） |
| `[ERR] hyp: undelegated guest page fault scause=…` | guest 自己的 stage-1 缺页没被委托到 VS，落到了 HS（`hyp_vcpu.c:958-964`） | 委托掩码没生效，**报 bug** |
| `[ERR] hyp: guest virtual instruction fault not emulated (pc=… stval=…)` | guest 在 VS-mode 做了模拟器不认的虚拟指令（`hyp_vcpu.c:945-954`） | 已知限制（`01-a20os-guest.md` §2.3 代价清单的最后一条） |
| `LOCK-STALL` 或 `MCS DEADLOCK` | **并发问题**。单 guest 槽是全局的，锁序有问题 | **报 bug**。带上完整日志。这两个串在 `smoke-hyp-a20os` 的 `forbid` 里（`smoke_cases.py:574`），门禁见到就红 |
| 第二个 `hypvm` 立刻失败（`FAIL vcpu_run`） | 单 guest 槽被占，`hyp_vcpu_run()` 抢锁失败回 `-EBUSY`（`hyp_vcpu.c:1000-1002`） | 等第一个结束。这是设计，不是 bug |
| 门禁 timeout（300 s 用尽） | guest 太慢，或卡在某个自旋 | 看日志最后停在哪；对照本表与 `01-a20os-guest.md` §6 |

### 5.2 日志在哪

- 门禁：`.kernel-build/smoke/hyp-a20os-riscv64.log`（`smoke_cases.py:554`）。
- 手动跑：`tools/smoke.py` 用 `run_with_timeout.py` 把 console 全量抓下来；
  自己起 QEMU 的话就是终端本身。

宿主与 guest 的输出在**同一根控制台**上，行会交错。看到一串看起来自相矛盾的
日志时，先想"哪几行是 guest 的"——唯一的可靠区分是看 `marker_seen` 与字节计数，
不是看行的样子。

---

## 6. 与其它文档的接口

- **要改设备模型**（guest 探到某个设备时该返回什么）→ [01-a20os-guest.md §6](01-a20os-guest.md)。
  那张行为表逐个寄存器列了 guest 侧在做什么、模型该做什么。
- **要改委托表或引导协议** → [01-a20os-guest.md §2 与 §7](01-a20os-guest.md)。
- **要问"为什么还没有 rootfs / 多 vcpu / 真机验证"** → [02-roadmap.md](02-roadmap.md)。
- **要改 stage-2 或 vcpu 运行循环** → [00-design.md](00-design.md)。
- **要改冻结契约** → `kernel/include/hyp/hyp_vcpu.h`，先读它的文件头。