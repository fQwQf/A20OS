# hypervisor 使用指南（把 A20OS 当 guest 跑起来）

最后核实：本文写作时 `user/cmds/core/hypvm.c` 与本篇**并行开发**，本篇按任务书
给定的签名先写，随后**已按落地的实现逐行核对**（§2.2 的参数表与 §3.1 的输出表
读的是 `hypvm.c` 本身）。若集成阶段实现再变，以实现为准并回来改本文。

标注约定沿用 [01-a20os-guest.md](01-a20os-guest.md)：**已验证** = 本片读过代码
或跑过命令并看到了输出；**未验证** = 设计推演或依赖尚未落地的实现，本片没有
跑过。

**本轮（最终门禁那一轮）改了什么**：新增 §3.4.2 与 `smoke-hyp-shell` 门禁（红），
把 §3.3 的"天花板"从上一轮的 `[SLAB BUG] kfree` 更新成本轮实测的
`[INIT] entering scheduler...` 内核栈页 use-after-free，并把 §3.4 的"怎么用 /
怎么退出"改成可照着敲的六步表。**"怎么让 guest 进 shell"这一问在本轮仍然是
没有答案的**——原因与证据见 §3.4.2，不要把 §3.4.1 的绿读成它。

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
运行，同一个镜像与内核，只有 `-cpu` 不同。**这三次是在压缩指令解码补上之前跑的，
所以 `console_bytes` 是 163；同一份树在补上之后重跑门禁是 17840（§3.3）——这两
个数之差是 guest 走得更远了，不是 `-cpu` 起的作用**：

| `-cpu` | `hyp_supported()` | 结果 |
| --- | --- | --- |
| `-cpu rv64,h=true` | 1 | `HYPVM: PASS marker_seen=1 console_bytes=163 exit=2(fault) mem=128 MiB` |
| `-cpu rv64`（即默认） | 1 | **同样 PASS**，输出逐字节相同 |
| 完全不写 `-cpu` | 1 | 同样 PASS |
| `-cpu rv64,h=false` | **仍然是 1** | **宿主 KERNEL PANIC**（不是 `-EOPNOTSUPP`） |

> 这张表是**一次旧测量**，`console_bytes=163` / `exit=2(fault)` 是补 RVC 解码**之前**
> 的那次，所以它没有 `rx_bytes=` 字段（契约 v3 之后才加的），四行"输出逐字节相同"
> 比的也是那时的格式。当前的字段清单见 §3.3 的行格式表，**不要拿这张表的样例去
> grep**。

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
事实——在 QEMU 10.0.13 上它去掉也过。**已验证**：`tools/smoke_cases.py:611` 起的
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

1. 用 `ARCH=riscv64 ABI=linux BRINGUP=0` 做 `dev-build`（`smoke_cases.py:614`）；
2. 把 `kernel-nosyms.elf` 放进镜像的 `/boot/guest-kernel.elf`；
3. 用 `-cpu rv64,h=true` 起 QEMU，等 shell 提示符 `# `；
4. 敲 `hyp_boot` 再敲 `poweroff`（`:558-560`）；
5. 断言 console 出现 `HYP_A20OS: PASS`，且**不**出现 `LOCK-STALL` /
   `MCS DEADLOCK` / `HYP_A20OS: FAIL`（`:573-575`）；
6. 日志落在 `.kernel-build/smoke/hyp-a20os-riscv64.log`。

timeout 是 **300 s** 而不是别的 case 常用的 60 s（`:562-565`）：guest 的每一个
console 字节、每一次页表走查、每一次二级缺页都要陷回宿主，guest 引导比宿主自举
慢好几个数量级。

**门禁的 `forbid` 刻意不含 `PANIC`**（`:566-572` 的注释）：门禁镜像不带
`RAMFS_USER=1`（`smoke_cases.py:614` 的构建变量），所以那个 guest 的根 ramfs 里
没有 `/bin/init`，会 panic 在 `init_kthread`（"init: no init program found"，
`kernel/main.c:361`），而 guest 的 panic 文本与宿主的逐字节相同——把 `PANIC` 放进
`forbid` 会让一次正确的运行判红。宿主真的死掉由"缺了 PASS 期望"抓住。
（`RAMFS_USER=1` 的 guest 就不一样了，见 §4.3。）

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

| 节点/属性 | 谁读 | 缺了会怎样（**本轮实测已更新**） |
| --- | --- | --- |
| 合法 FDT 头（version 17） | `riscv64_memory_init()` | 打 `[FDT] memory node parse failed, using board window …` 后回退到板级窗口。**本轮实测就是这条**（`kernel/arch/riscv64/platform/fdt.c:295`） |
| `/memory@<base>/reg` | `riscv64_memory_init()` | 同上，回退到 `0x80000000 .. +18 GiB` 的板级窗口，**比我们要的 128 MiB 大得多** |
| `/cpus/timebase-frequency` = 10000000 | `riscv64_fdt_timebase_freq()` | 回退到 arch 常量（qemu-virt 上也是 10 MHz），只是把回退值钉死。本轮实测打的是 10 MHz，与本属性同值，看不出是哪一条给的 |
| `/cpus/riscv,isa` 含 `sstc` | `riscv64_fdt_has_isa_extension()` | **本轮实测：guest 读成了"没有"**，于是走 SBI `set_timer`。此前这里写的是"致命：v2 的 SBI 面没有 set_timer，guest 死在 `[INIT] Timer initialized`"——**两句都已被推翻**：set_timer 实现了（`hyp_vcpu.c:386-388`），而 guest 不但没死，还活着走完了整个内存子系统 |
| `/chosen/bootargs` | `arch_bootargs_get()` | **本轮实测：打 `[FDT] no bootargs extracted`**（`fdt.c:313`），尽管 bootargs 已经被合成进去（`hyp_guest.c:406-411`） |

后三行都是同一个方向的失败：guest 侧没能读出这份合成 FDT 的属性。
**待查**（本片没有定位根因）。它在实践里的后果是具体的：`-b` 写的
`a20.hypguest=1` 到不了 guest，guest 的内存窗口按板级默认值算（18 GiB 而不是
`-m` 给的 128 MiB），guest 的时器按 SBI 后端走。

isa 串固定写 `rv64imafdch_sstc`（`hyp_guest.c:411`），**不受 `-b` 影响**——这是
承重件，不是可配项。

> **`-b` 现在是"写进去了"而 guest 还没读到。** 属性确实被合成了（`hypvm` 的回执
> 行原样回显它，`smoke-hyp-vm-96` 就是靠这一行断言它没被静默丢掉），但本轮实测
> 里 guest 打的是 `[FDT] no bootargs extracted`（`fdt.c:313`）——**这条消息出现了，
> 而且出现的位置正是本节此前说"从未出现"的那一步**。所以 `-b` 现在的状态是
> "宿主合成了，guest 侧的 FDT 解析没取到"，原因见 §2.3 表下面那段。
> 更早一版的本文说"日志里没有一行 `[FDT]`"是当时 guest 在更早的地方就 fault
> 退出造成的，现已不成立。

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
| `HYPVM: exit=<n>(<name>) scause=0x… stval=0x… htval=0x… marker_seen=<n> console_bytes=<n> rx_bytes=<n>` | `hypvm.c:372-379` | 运行结束。`<name>` 是 `none`/`shutdown`/`fault`/`error`（`hyp_vcpu.h:53-58`）；`scause`/`stval` 是 guest 的寄存器值，**`scause` 用 `%lx` 打的，是十六进制**（见 §3.4.1）；`htval` 是二级缺页的 GPA **右移 2 位**的原始值（`hyp_vcpu.h:100`），**真实 GPA 是 `htval << 2`**；**`rx_bytes` 是本轮新增字段**（契约 v3），宿主交给这个 guest 的输入字节累计，见 §3.4 |
| `HYPVM: PASS marker_seen=<n> console_bytes=<n> rx_bytes=<n> exit=<n>(<name>) mem=<n> MiB` | `hypvm.c:392-398` | 判据通过。五个数一并打出来，让人不用回头翻上一行 |
| `HYPVM: FAIL <what> (rc=… errno=…)[: <说明>]` | `hypvm.c:111,119` | 失败。`<what>` 是失败环节；带 `: <说明>` 的那批来自 `fail_call()`，额外说明这个 errno 是不是"内核不支持" |
| `HYPVM: FAIL <具体消息>` | 各处 | 参数/布局类的具体拒绝理由，逐条见 §5.1 |

**注意 `HYPVM: FAIL` 的退出码不都是 1。** 见 §2.2 的退出码表：`vm_create` 之类
的系统调用被内核拒绝（多半是没有 H 扩展）回的是 **4**，不是 1。脚本里判"guest
跑了但没到"要看 1，判"这台机器根本不支持"要看 4。

宿主内核侧还会打一些 `[ERR] hyp: …` / `hyp: …` 行，它们**不是** `hypvm` 的
输出，但排查时要看（见 §5.1 与 §5.2）。

**本轮新增的用户可见输出字段只有一个：`rx_bytes`。** 它是契约 v3 的东西，
内核侧落在 `struct hyp_vm_status` 的末尾（`kernel/abi/linux/sys_a20_bridge.c:508`，
赋值在 `:547`；用户侧镜像 `user/cmds/core/hyp/hyp_guest.h:50`），`hypvm` 现在把它
打进了 `exit=` 与 `PASS` 两行（`hypvm.c:372-379` 与 `:392-398`）。**除此之外
`hypvm` 的输出行集合一个字没变**——没有新行、没有新前缀。
`rx_bytes` 怎么读、它能证明什么，见 §3.4。

**一次成功运行的输出骨架**（**已验证，本轮实跑 `make smoke-hyp-a20os`**：退出 0、
`smoke-hyp-a20os: PASS`，日志 573 行。下面这份骨架逐行取自本轮那份日志，中间的
PCI 枚举与宿主 trap 轨迹省略）：

```
HYP_A20OS: guest=/bin/boot/guest-kernel.elf size=4618832 dtb=288
HYP_A20OS: segments=3 entry_gpa=0x80200000 image_end=0x810e0914 ram=128 MiB marker='A20OS Kernel'
HYP_A20OS: running
    A20OS Kernel                 ← guest 自己的横幅，宿主也打过一模一样的一行
Initializing system...
UBSAN_SELFTEST: PASS
[INIT] Trap initialized
[FDT] memory node parse failed, using board window 0x80000000..0x480000000
[INIT] UART initialized
[TIMER] backend=sbi freq=10000000 Hz
[INIT] Timer initialized
[INIT] Timekeeping initialized
[MM] Buddy+Slab: 4194304 frames, 4173459 free (16302 MB)
[INIT] Memory initialized
[FDT] dtb_ptr=0x87f00000
[FDT] no bootargs extracted
[INIT] Boot arguments parsed
[BUS] virtio-mmio: found 0 devices (base=0xffffffc010001000 irq_base=1)
[INIT] Drivers probed
[INIT] USB devices scanned
[WARN] hyp: guest MMIO at 47fac00a7 x8 times (no model)
[RAMFS] Initialized, root inode 0
[SLAB BUG] kfree(0xffffffc47fae0010): invalid non-slab pointer hdr=… magic=0x0 …
[PANIC] attempting firmware poweroff
HYP_A20OS: exit=1(shutdown) scause=0xa stval=0x0 htval=0x0 marker_seen=1 console_bytes=17840
HYP_A20OS: PASS
```

**当前树里没有调试打印**，所以这份骨架是**当前日志**而不是上一轮的：

```
$ grep -rn DBG kernel/hyp/ kernel/arch/riscv64/hyp/ ; echo "rc=$?"
rc=1
$ git show HEAD:kernel/hyp/hyp_vcpu.c | grep -c DBG
0
$ wc -l .kernel-build/smoke/hyp-a20os-riscv64.log
573 .kernel-build/smoke/hyp-a20os-riscv64.log
$ grep -c DBG .kernel-build/smoke/hyp-a20os-riscv64.log
0
```

（上一轮树里 `kernel/hyp/hyp_vcpu.c` 确实有过两处 `DBG decode` / `DBG served` 打印，
把日志撑到 43 万行；那份日志已经不存在了，历史记在
[01-a20os-guest.md](01-a20os-guest.md) 的"第二次"一段。别把那段当当前状态。）

上面这条是 `hyp_boot` 的字面输出（门禁按字面匹配，所以它没被改过）。同样的运行
用 `hypvm` 时，前三行换成 §3.1 表里的 `HYPVM:` 三行、最后两行换成带 `mem=` 的版本。

`elf_bytes`/`size` 的绝对值会随镜像变化（本轮实测 4618832，本文早期写的是
4618448），不要把它当判据。`dtb_gpa=0x87f00000` 才是稳定的那个：它正是 §2.3 那条
"顶端下量 1 MiB"算出来的值（`0x88000000 - 0x100000`）——**恰好等于** `hyp_boot`
硬编码的 `GUEST_DTB_GPA`（`hyp_boot.c:87`），所以两个工具在默认参数下把 DTB 放在
同一个地址。

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

### 3.3 guest 现在的终点：死在 RAMFS 交界处的 `kfree`，`exit=1(shutdown)`

**这一节写过两次结论，分属两棵不同的树。当前树是下面"第三次"那种，
前面的"第一次/第二次"只当历史读**（完整的三次记在
[01-a20os-guest.md](01-a20os-guest.md)）。

**第一次（更早的树，日志 573 行，可读）**：guest 走完 banner 到
`[RAMFS] Initialized` 的整条启动路径，`console_bytes=163 → 17840`，然后在自己的
`kfree` 上报 `[SLAB BUG] invalid non-slab pointer`，`KERNEL PANIC`，再由
`[PANIC] attempting firmware poweroff` 发 SBI SRST ecall，于是宿主看到
`exit=1(shutdown)`。**与 `PASS` 同时出现。**

**第二次（带在途调试打印的树）**：结果行逐字段相同，但日志被
`kernel/hyp/hyp_vcpu.c` 里两处 `DBG` 打印撑到 43 万行，guest 段读不出来，
看上去像是"停在 UART 页上"——

```
[ERR] hyp: trap loop at pc=ffffffc080209664 scause=17 (1246 traps so far)
```

**第三次（当前树，`make smoke-hyp-a20os` 退出 0、`smoke-hyp-a20os: PASS`，日志
573 行、`grep -c DBG` = 0）**：那次自旋**是调试打印造成的假象**。去掉打印重跑，
guest 停在和第一次一模一样的地方——`[INIT] USB devices scanned` 之后的
`[RAMFS] Initialized`，然后在 `kfree` 一个 RAMFS 指针上 panic。当前日志里**没有**
`trap loop` 那一行（`grep -c 'trap loop'` = 0），取而代之的是清清楚楚的三行：

```
557: [SLAB BUG] kfree(0xffffffc47fae0010): invalid non-slab pointer hdr=0xffffffc47fae0000 magic=0x0 order=0 ra=0xffffffc08033c284
558: [SLAB BUG]   pfn=4192992 flags=0x1 refcount=1 order_meta=5 cpu=0
559: [SLAB BUG]   tlb_flush_calls=0 tlb_ipi_serviced=0
563: ========== KERNEL PANIC ==========
571: [PANIC] attempting firmware poweroff
572: HYP_A20OS: exit=1(shutdown) scause=0xa stval=0x0 htval=0x0 marker_seen=1 console_bytes=17840
573: HYP_A20OS: PASS
```

`exit=1` 是 `shutdown` 不是 `fault`（`HYP_EXIT_SHUTDOWN`，
`kernel/include/hyp/hyp_vcpu.h:53-58`），但**它不等于"guest 干净地关机了"**：
`scause=0xa` = 10，本树把它定义为 ecall 路径的一个成员
（`HYP_SCAUSE_ECALL_S`，`kernel/hyp/hyp_vcpu.c:67`；分派器把 8/9/10 三个一起收，
`hyp_vcpu.c:1115-1119`，所以是哪一位不影响结论），由 `hyp_guest_ecall()` 的
`eid=SRST fid=0` 分支接住（`hyp_vcpu.c:413-418`）。
**判"guest 有没有走完它该走的路"要看 panic 之前那几行，不是看 `exit=`。**

`flags=0x1 refcount=1` 说明那一页在 buddy 层仍是"已分配"的，而被 `kfree` 的地址
落在 `0xffffffc47f...`——guest 的 RAMFS 区间。**本片没有定位根因，也没有证据说它
与 stage-2 有关**：解码零缺口（`grep -c "undecodable guest access"` = 0）、guest 一路
走到 `[INIT] USB devices scanned`、该走的二级页错配都走完了。`RAMFS_USER=0` 的镜像
里没有 init，`kernel/main.c:359-361` 的 `panic("init: no init program found")` 是
另一条路，也不是这一条。

当前日志里 guest 侧 16550 的访问痕迹只剩启动阶段那几对（`htval << 2` 是
`0x10000005` 与 `0x10000000`，即 LSR 与 THR，`01-a20os-guest.md` §6.1）：

```
233: [ERR] hyp: trap #3 scause=15 pc=ffffffc0803f99e0 stval=ffffffc010000005 htval=4000001
234: [ERR] hyp: trap #4 scause=17 pc=ffffffc0803f99ea stval=ffffffc010000000 htval=4000000
```

**读法**：`scause=15` 是 load（读 LSR），`scause=17` 是 store（往 THR 写），成对出现
是 `arch_uart_putc()` 等 THRE 的自旋，不是自旋等输入。当前树里没有配套的 `DBG decode`
行，所以这一对**只能从 `htval` 认出是哪个寄存器，看不到指令本身**——别照抄"第二次"
那一版里 `insn=44783` / `insn=13a0023` 的读法，那是带调试打印那次的产物。

**现在能确定的三件事**：

1. **`exit=N` 与 `PASS` 是两件事。** `PASS` 只证明 marker 命中（§3.2），不证明
   guest 走得健康、不证明它没 panic、不证明它没自旋。
2. **读 `hypvm` / 门禁的输出要读到最后一行之上最近的那条异常**
   （`[PANIC]`/`[SLAB BUG]`/`trap loop`/`undecodable guest access`），**而不是读
   PASS**。带调试打印那次读到的是 `trap loop`，当前树读到的是 `SLAB BUG`——
   两次的 `exit=` 与 `console_bytes` 一模一样。
3. **RVC 解码确实在起作用**：guest 能走到"轮询 LSR"这种需要访存指令的代码，
   就是靠 `hyp_decode_rvc_access()`（`kernel/hyp/hyp_vcpu.c:611-714`，解 8 种访存
   形式 `c.lw`/`c.ld`/`c.sw`/`c.sd`/`c.lwsp`/`c.ldsp`/`c.swsp`/`c.sdsp`），入口按
   编码长度分流（`hyp_vcpu.c:716-724`），被服务过的访存按**编码长度**推进 `sepc`
   而不是固定 +4（`hyp_vcpu.c:866-872`，16 位编码推进 2）。上一轮 guest 死在一条
   16 位 `c.sw` 上；本轮它走得更远。刻意不含的是 `c.fldsp`/`c.fsdsp`：guest 的
   `sstatus` 带 FS Off，浮点指令会先被 guest 自己的非法指令陷阱截住，解它够不到
   （`hyp_vcpu.c:604-609` 的注释给了理由与实测计数）。

解不出来时的那行消息带原始机器码与一个 ` rvc` 后缀：

```
[ERR] hyp: undecodable guest access at pc=<pc> (insn=<hex>[ rvc])
```

（`hyp_vcpu.c:826-835`。）` rvc` 只表示"这条是 16 位编码"，不表示它被解出来了。
**本轮的日志里没有这一行**——这也是"解码器补上了"的一个反面证据。

**天花板在哪。** 这一句在本片**已经被实测推翻了两次**，所以这里记的是第三次的
位置，别再照抄旧版本：

- **第一次的天花板**是解码器：`undecodable guest access at pc=ffffffc080445ada`。
  补上 RVC 之后 `grep -c "undecodable guest access"` = **0**，这条线彻底没了。
- **第二次的天花板**被误判成"guest 自己的 UART 轮询自旋"
  （`trap loop at pc=ffffffc080209664 scause=17`）。**那是调试打印刷爆日志造成的
  假象**：43 万行日志里 guest 那几行根本读不出来。去掉调试打印后重跑，guest 停在
  和第一次一模一样的地方：`[RAMFS] Initialized` 之后的
  `[SLAB BUG] kfree(0xffffffc47f...): invalid non-slab pointer`。
- **第三次**：`RAMFS_USER=1` 的 guest 与上面一模一样——`[RAMFS] Initialized`
  之后 `kfree` 一个 RAMFS 指针，`console_bytes=17840`。**加上 `RAMFS_USER=1` 本身
  并没有把 guest 推得更远**。
- **第四次（本轮 `make smoke-hyp-shell` 实跑，日志
  `.kernel-build/smoke/hyp-shell-riscv64.log`）**：**失败点前移了，而且换了一个
  形状**。guest 这次打完了整个 banner（`console_bytes=20926`），走到
  `[INIT] entering scheduler...`，然后死在**内核栈指针被判定为损坏**：

  ```
  605: Welcome to A20OS!
  607: [INIT] entering scheduler...
  [ERR] [TRAP] corrupted kernel sp detected[ERR] [TRAP]   bad_sp=0xffffffc087f7eec0 ...
  [ERR] [TRAP]   pfn 32638 sits on buddy free list — use-after-free of stack page
  [ERR] [TRAP]   current: pid=1 tgid=1 ppid=0 kstack_base=0xffffffc087fa0010 kstack=0xffffffc087faff90 state=2
  ========== KERNEL PANIC ==========
  trap: corrupted kernel stack pointer
  ```

  `[RAMFS] Initialized` / `[SLAB BUG]` 这一次**都没出现**（`grep -c 'SLAB BUG'`
  = 0），而 `pid=1 name=kthread` 说明**guest 连第一个用户任务都还没建**（宿主自己
  那份 `[init] forking...` / `execve mksh` 在日志第 223-224 行，是宿主的，别读成
  guest 的）。所以"上一轮 `[SLAB BUG]` 的那个 `kfree`"已经不是当前的天花板了。

所以现在真正的天花板是 **guest 在 `[INIT] entering scheduler...` 处对自己内核栈页
的 use-after-free**，不是设备模型的边界——设备模型对 UART 页是有模型的
（`kernel/hyp/hyp_dev.c` 的 `hyp_uart_read_reg()` / `hyp_uart_write_reg()`，
`:283-318`），`RAMFS_USER=1` 的 guest 内核也已经打进了 FAT32 镜像
（`/boot/guest-kernel-ramfs.elf`，`tools/targets-images.mk` 的
`$(GUEST_KERNEL_RAMFS_ELF)`）。真 rootfs 仍然要 virtio，见 `02-roadmap.md` §4。

> **本片没有定位上面那个栈页 use-after-free 的根因，也没有证据说它与 stage-2
> 有关。** 能说的只有：guest 已经走完了 banner、`UBSAN_SELFTEST: PASS`、FDT 解析、
  slab/pfa/页表初始化、ramfs、以及走进调度器的那一步；解码零缺口
  （`grep -c "undecodable guest access"` = 0）。**不要把上一轮的 `kfree`/`SLAB BUG`
> 结论照抄到这一轮**——它已经被这一轮的日志推翻了。


---

### 3.4 和 guest 交互：控制台输入（契约 v3，本轮新增）

**先说清今天做到哪一步，再教怎么用**，因为这两件事差得很远：

- **通道是通的**：你在 `hypvm` 跑着的时候敲的字，宿主 UART 收到之后被搬进这台
  guest 自己的 16550 收字节环。这是设备模型里的一条真路径，计数器 `rx_bytes`
  量的是它。
- **真 A20OS guest 走到 shell 之前就死了，所以它读不走这些字节**。本轮
  `make smoke-hyp-shell` 实跑（`.kernel-build/smoke/hyp-shell-riscv64.log`）**泵是通的**：
  `rx_bytes=18`，正好是 `echo AAAABBBBCCCC\n` 那 18 个字节；但那两个 token
  在日志里**一次都没出现**（`grep -c AAAABBBBCCCC` = 0），guest 停在
  `[INIT] entering scheduler...` 上的内核栈页 use-after-free（§3.3 第四次），
  **连 `/bin/init` 都没 exec**，而读 RBR 的代码在 mksh 之后。上一轮那次也是同样
  的形状：`smoke-hyp-console` 的日志里 guest 全程零次 RBR 读
  （`grep -c 'scause=15.*stval=ffffffc010000000'` = 0；同一偏移上的 10 次全是
  `scause=17` 的 THR 写，另有 11 次 `scause=15` 落在 `stval=…005` 即读 LSR）。
- **往返只在合成 guest 上被证明**。`smoke-hyp-console-p0` 里的 guest 是
  `hyp_test echo` 那 13 条指令，它自己就是那个轮询 `LSR`、弹 `RBR` 的读者，所以
  `HYP` 三个字符能原样回来（§3.4.1）。这条证据证明的是**设备模型这一段**，
  不是"A20OS-as-guest 能交互"。
- **被偷走又没人读的按键，在 VM 销毁时静默丢弃**（`hyp_dev_pump_rx()` 的注释
  写着 "STOLEN, NOT COPIED"）。本轮那 18 个字节就是这么没的，日志里看不出来——
  `rx_bytes` 只数"进了环"，不数"被读了"。

**所以"怎么用"要分两种 guest 读**：

- **想验证通道本身**：跑 `make smoke-hyp-console-p0`，或在 shell 里敲
  `hyp_test echo`，看到独立一行 `HYP` 就是通的。
- **想让 guest 进 shell 并敲命令**：本轮新写的门禁 `make smoke-hyp-shell` 就是
  这件事的完整配方（**当前是红的**，见 §3.4.2）。手工做就是同一串：

  | 步骤 | 敲什么 | 为什么是这个时机 |
  | --- | --- | --- |
  | 1 | `hypvm -k /bin/boot/guest-kernel-ramfs.elf` | 必须带 `-k`：`RAMFS_USER=1` 的 guest 内核是**另一个 ELF**，不带会静默回落到默认内核 |
  | 2 | 等 `HYPVM: running` | 这行是 hypvm 把 CPU 交给 guest 的前一刻（`user/cmds/core/hypvm.c:354`）。**在这之前敲的字已经被宿主 shell 自己吃掉了**——那时 pump 还没开始跑 |
  | 3 | 敲 `echo AAAABBBBCCCC` 回车 | 字节落进宿主 rx ring，下一次 guest trap 的 `hyp_dev_pump_rx()` 搬进 guest 的环 |
  | 4 | 看**命令输出**里有没有 `AAAABBBBCCCC` | 这一步才是"guest 真的读了"的判据，见下面"怎么判断交互真的通了"第 2 条 |
  | 5 | 敲 `exit` 回车 | **只能用 `exit`，不能敲 `poweroff`**，理由见下 |
  | 6 | `HYPVM: PASS ...` 出现后，敲 `poweroff` | 这一下是给**宿主**的。pump 只在 `hyp_vcpu_run()` 里面跑，guest 一走按键就回到宿主 shell |

- **怎么退出**：**在 guest 里敲 `exit`**。mksh 退出后 `user/init.c:264-283` 的
  `waitpid` 循环收掉 shell 子进程、跳出循环、调 `do_shutdown()`，后者打一行
  `[init] shutting down` 再 `reboot(RB_POWER_OFF)`（`user/init.c:137-143`）；
  guest 内核把它变成 SBI SRST shutdown，宿主在 `hyp_guest_ecall()` 里接成
  `HYP_EXIT_SHUTDOWN`（= 1，`kernel/include/hyp/hyp_vcpu.h:55`），`hyp_vm_run()`
  这才返回、宿主 shell 才拿回提示符。
  **不能给 guest 敲 `poweroff`**：guest 的 rootfs 就是
  `RAMFS_USER_PROGRAMS` 那十来个程序（`Makefile:1238-1241`），里面**没有**
  `poweroff` 这个二进制，而 guest 背后没有块设备可以去找它。
  Ctrl-C 也到不了 guest（见本节后面的限制），所以唯一的退出键就是 `exit`。


这条路的形状是"偷"，不是"转发"，理解它才能预测行为：

| 环节 | 事实 | 依据 |
| --- | --- | --- |
| 谁搬字节 | guest trap 分派器的**第一件事**就是 `hyp_dev_pump_rx(vcpu->vm)`，在宿主 IRQ 那条早退**之前** | `kernel/hyp/hyp_vcpu.c:1072`；为什么必须在那之前，`hyp_vcpu.c:1052-1071` 的注释 |
| 从哪搬 | `uart_try_getc()`，宿主 UART 的 rx ring，**非阻塞**。`uart_getc()` 绝不能出现在这条路上（它会 park，guest trap 里的睡眠没人唤醒） | `kernel/hyp/hyp_dev.c:204-250`（`uart_try_getc()` 在 `:243`） |
| 搬到哪 | VM 上的一个 256 字节环形缓冲，单生产者单消费者、一格松弛（`head == tail` 是空，所以 256 字节装 255） | `kernel/include/hyp/hyp.h:45`（`HYP_VM_RX_RING`）与 `:70-86` 的字段块 |
| guest 怎么知道有输入 | **轮询 `LSR`**。bit 0（DR）随环里有没有字节变，所以 LSR 读回 `0x60`（空）或 `0x61`（有输入）。bit 5/6 恒定——那正是让 guest 打印不等待的那两位 | 常量 `hyp_dev.c:69-70`，合成处 `hyp_dev.c:286-289` |
| guest 怎么取 | 读 `RBR`（偏移 0x0）弹**一个**字节。一次访问只弹一个，宽读不会一次吞掉多个按键 | `hyp_dev.c:290-304`（`byte_index == 0` 才弹） |
| **中断在哪** | **没有。** `IIR` 仍恒读 `0x01`（无中断待处理），全树也没有一处写 `hvip` | `hyp_dev.c:71-76,286`；契约 v3 段 `kernel/include/hyp/hyp_vcpu.h:247-274` 把这条写成了"两半才成一件改动" |

**怎么判断"交互真的通了"**，按可靠度从高到低。**注意第 2 条只对
`smoke-hyp-console-p0` 那种合成 guest 成立**——真 guest 今天做不到：

1. **`rx_bytes > 0`**：宿主交给这个 guest 的字节总数。走
   `SYS_hyp_vm_status`，它落在 `struct hyp_vm_status` 的**最后一个**字段
   （`kernel/abi/linux/sys_a20_bridge.c:508,547`；用户侧镜像
   `user/cmds/core/hyp/hyp_guest.h:50`）。**`hypvm` 现在打这一行**——在 `exit=` 与
   `PASS` 两行里各有一个 `rx_bytes=`（`user/cmds/core/hypvm.c:372-379,392-398`），
   所以交互跑完直接看 `PASS` 那行的 `rx_bytes` 就行，不用另写程序。
   门禁 `smoke-hyp-console-p0` 的第三条期望 `r'rx_bytes=[1-9][0-9]*'`
   就是它；`hyp_test` 也打同一行（`user/cmds/core/hyp_test.c`）。
   **它证明的是"字节进了 guest 的收字节环"，不是"guest 读到了"**——这是两件事。
2. **guest 自己执行了你敲的命令、并把输出打回来**。guest 的 UART 写仍然走 THR
   到宿主控制台，所以 guest 打印出来的字符会直接出现在同一行控制台上。
   **敲一条 guest 会执行的命令（`echo <某个 token>`），看那个 token 的输出有没有
   出现**——这是不写代码就能做的判据，也是 `smoke-hyp-shell` 的主体判据。
   **注意区分"回显"和"输出"**：宿主控制台自己的行是 CRLF 结尾的
   （实测日志第 226 行 `# hypvm -k ...^M`），guest 走 THR 出来的行不一定是；
   所以判据要认住**输出 token 单独成行**，而不是"日志里出现过这个 token"。
   **本轮这条仍然是红的**：两个 token 一次都没出现在
   `.kernel-build/smoke/hyp-shell-riscv64.log` 里，见 §3.4.2。
   合成 guest 上这条早就成立（`hyp_test echo` 把你敲的三个字节 putchar 回来）。
3. **`console_bytes` 在涨**：只能证明 guest 在打印，**不能**证明有输入进来。两者
   是不同的计数器（出 vs 进），别混。

**限制，每条都会咬人**：

- **单读者。** 宿主 rx ring 只有一个消费者，这里在偷它。`hypvm` 跑着的时候宿主
  shell 正阻塞在系统调用里不读，所以"偷的人是唯一的读者"目前成立。
  **`CONFIG_NR_CPUS>1` 时这条立刻不成立**——症状是丢按键。写在
  `hyp_dev.c:216-222` 的注释里，不是猜的。
- **纯轮询。** guest 的驱动必须轮询 `LSR`。走中断的 guest 会永远等不到。
- **偷走的、没人读的按键就丢了。** `hyp_dev_pump_rx()` 用的是 `uart_try_getc()`
  （弹出），不是复制，所以 VM 销毁时环里剩的字节跟着环一起消失。本轮真 guest 那 18 个
  字节就是这么没的。想复现"按键消失"，不要指望日志会抱怨：`hyp_dev_pump_rx()`
  的搬运循环**一行日志都没有**，只有两个 `break` 的注释，唯一的计数是 `rx_bytes`，
  而它落在 `HYPVM: ` 那一行上、**不区分"被 guest 读了"还是"被丢了"**。
  **这正是本轮把 `smoke-hyp-shell` 的两条判据分开写的原因**：`rx_bytes` 管"进了环"，
  命令输出管"被读了"。
- **`poweroff` 会和 guest 抢同一个 rx ring。** 用 `sendline`（把命令一次性排进
  stdin）跑 `hypvm` 时，紧跟着 `hypvm` 的那行 `poweroff` 会在 guest 跑着的时候被
  **整行搬进 guest 的环**然后丢掉：`smoke-hyp-vm` / `smoke-hyp-vm-96` 的日志里
  `rx_bytes=9` 正好是 `poweroff\n` 那 9 个字节，两条门禁都是靠 300 s 超时被掐掉的
  （它们仍然报 PASS，因为 `smoke.py` 的 `report()` 只看日志文本、不看 QEMU 的退出
  码）。`sendline_seq` 的门禁把最后一步挂在程序自己的 `PASS` 行上，避开这个洞。
- **没有第二块输入通道。** 交互和宿主 shell 抢同一个控制台：你想给 guest 打字，
  就不能同时给宿主 shell 下命令。`smoke-hyp-a20os` 之所以能跑，是因为它在
  `hypvm`/`hyp_boot` 之前就把要敲的行排进了 stdin。
- **`Ctrl-C` 到不了 guest。** `uart_rx_push()` 在进 ring 之前就把 `0x03` 截去当
  SIGINT 了（`kernel/drivers/char/uart.c:151`），理由写在
  `kernel/hyp/hyp_dev.c:232-236` 的注释里。所以**你没法用 ^C 打断一个跑飞的
  guest**，只能从 QEMU 那边掐掉整台虚拟机。这也是上面"怎么退出"那条只能给
  两个答案的原因。

### 3.4.1 门禁 `smoke-hyp-console-p0`：往返**通了**，门禁**绿**

本轮新增了一条专门验收这条通道的门禁：`smoke-hyp-console-p0`
（`tools/smoke_cases.py:563-596`）。它用 `sendline_seq` 分三步往 QEMU 的 stdin 里
塞：先等 shell 提示符敲 `hyp_test echo`，再等 guest 起来，**然后才送那三个字节**——
顺序是门禁的主体，因为字节必须在 guest 有机会去轮询**之前**躺在宿主 rx ring 里
（`smoke_cases.py:539-551` 的注释给了理由）。

**已验证（本片实跑 `make smoke-hyp-console-p0`）**：往返本身是通的。日志
`.kernel-build/smoke/hyp-console-p0-riscv64.log` 第 227-228 行：

```
227: HYP_VCPU_TEST: guest up, send 3 bytes
228: HYP
```

那行独立成行的 `HYP` 就是三个字节从宿主控制台进、`guest` 轮询 `LSR` 读到 DR、
弹 `RBR` 取出、再经 SBI `console_putchar` 打回宿主控制台的**结果**。这条路径上
只有设备模型。

**当前树这次是过的**，完整尾部是：

```
227: HYP_VCPU_TEST: guest up, send 3 bytes
228: HYP
229: [ERR] hyp: trap #0 scause=15 pc=80000038 stval=10000005 htval=4000001
230: [ERR] hyp: trap #1 scause=15 pc=80000044 stval=10000000 htval=4000000
231: H[ERR] hyp: trap #2 scause=15 pc=80000038 stval=10000005 htval=4000001
232: [ERR] hyp: trap #3 scause=15 pc=80000044 stval=10000000 htval=4000000
233: Y[ERR] hyp: trap #4 scause=15 pc=80000038 stval=10000005 htval=4000001
234: [ERR] hyp: trap #5 scause=15 pc=80000044 stval=10000000 htval=4000000
235: PHYP_VCPU_TEST: exit=1 rx_bytes=4 console_bytes=0
236: HYP_VCPU_TEST: PASS
```

三对 trap 就是三轮：`LSR`（`pc=80000038`，`stval=10000005`）与 `RBR`
（`pc=80000044`，`stval=10000000`），各弹出一个字节；`HYP` 三个字符是 guest 自己
`putchar` 打回来的。`exit=1` 是 SBI shutdown，所以 `want=1` 那条也满足了。

`rx_bytes=4` 比送进去的 3 个多一个：门禁用 `sendline_seq` 发的是一整行，行尾那个
换行也是宿主 rx ring 里的一个字节，pump 一起搬走了；guest 凑够三个就关 SBI，
第四个没人读。`hyp_test` 里的判据是 `st.rx_bytes < GUEST_ECHO_N` 的**下界**，
所以这是设计内的。

> **上一版这里记的是"门禁红"，那个红已经定位并修掉了，但它跟设备模型无关。**
> `user/cmds/core/hyp_test.c` 里那行
> `memcpy(code + n, guest_echo, sizeof(guest_echo) / sizeof(guest_echo[0]))`
> 把**元素个数当成了字节数**：`memcpy` 的第三个参数是字节数，于是拷了 13 字节 =
> 三个整字加第四个字的头一个字节。guest 于是把 `andi a1,a1,1`（`0x0015f593`）取成
> `0x00000093`，解成 `addi ra,x0,0`（本身无害），带着**过期的** `LSR` 字节走 `beq`
> 分支，跳进镜像后面的零填充，在 `pc=0` 取指失败。
>
> 这条值得单独记下来的原因是它的失败形状：**编译不报、运行不报、头一个字还对**。
> `hyp_decode_guest_access()` 解出来的 `va`/`gpa`/`rd`/`ilen` 一项不差，头三条 trap
> 全是"正确的"二级缺页，唯一可见的症状在四步之外。定位它靠的是把 guest 内存按页
> 倒出来——倒出来的 `code[15] = 0x00000093` 才是答案，而同一时刻解码器正在报的
> `insn` 是完全正确的 `0x00574583`。

**所以：交互能力已落地，往返已实测（**在合成 guest 上**），门禁绿。** 那次红的是
**镜像装载**（拷了 13 字节而不是 52），不是通道、也不是设备模型——`LSR` 的 DR 位、
`RBR` 的弹出语义、`hyp_dev_pump_rx()` 的泵点三处从头到尾没改过。

**这条门禁只覆盖设备模型那一段。** 它的 guest 是 13 条指令的 `hyp_test echo`，
不是 A20OS-as-guest；真 guest 上"按键进了环但没人读"这件事由
`smoke-hyp-console` 单独记，两条门禁的分工写在 §3.4 开头。

**收尾也在这条门禁的断言里。** 第三个 `sendline_seq` 步骤挂在
`HYP_VCPU_TEST: PASS` 上而不是 shell 提示符上，因为 `hyp_test` 在打这行之前已经
`SYS_hyp_vm_destroy`，而 pump 只在 `hyp_vcpu_run()` 里面跑。挂在提示符上的那一版
实测收尾是 `# oweroff` / `E: mksh: oweroff: inaccessible or not found`，QEMU 靠
120 s 超时被掐掉而门禁照样 PASS——`smoke.py` 的 `report()` 只看日志文本、不看 QEMU
的退出码。现在的 `expect` 里多了一条 `System is going down for power-off NOW`，
就是补这个洞。

> `scause=14` 是**十六进制**，0x14 = 20 = guest 取指缺页（QEMU 10.0.13 的
> `RISCV_EXCP_INST_GUEST_PAGE_FAULT = 0x14`，`target/riscv/cpu_bits.h:720`），
> 对应树里的 `HYP_SCAUSE_GPF_INST`（`kernel/hyp/hyp_vcpu.c:71`）。别读成十进制的
> 14——那是规格里的 Reserved，会把结论带偏。同一条日志里的 `scause=15`/`17` 是
> 0x15/0x17 = 21/23，即 load/store guest access fault（`cpu_bits.h:721,723`）。

### 3.4.2 门禁 `smoke-hyp-shell`：**本轮新增，当前是红的**

这是**验收"guest 能进 shell 并且能交互"的那一条**，也是 §3.4 那张"怎么用"表
的可执行版本（用例在 `tools/smoke_cases.py:810-962`，注册在
`tools/targets-smoke.mk:202-213`）。

**它和 `smoke-hyp-console-p0` 的分工不是"更严格的同一条"，是两条不同的主张**：

| 门禁 | guest 是谁 | 断言的是 | 当前 |
| --- | --- | --- | --- |
| `smoke-hyp-console-p0` | 13 条指令的 `hyp_test echo`，**它自己就是 RBR 读者** | 设备模型这一段：字节进环、弹出、再打回来 | **绿** |
| `smoke-hyp-console` | 真 `RAMFS_USER=1` 内核 | 只有 `rx_bytes` 非零（**不声称 guest 读了**） | 绿，但那条主张很弱 |
| **`smoke-hyp-shell`** | **真 `RAMFS_USER=1` 内核 + 它的 `/bin/init` + mksh** | **guest 到了 mksh 提示符、敲进去的命令被执行、输出打回来、且 `rx_bytes` 非零** | **红** |

**两个主张必须分开断言，这一点是上一轮被评审抓到的失实点。** `rx_bytes` 只在
`hyp_dev_pump_rx()` 里、只在 `uart_try_getc()` 真拿到字节时递增，它证明的是
**"宿主把键交到了 guest 的收字节环里"**；它**完全不区分这些字节后来被 guest 读了
还是被丢了**。所以本条门禁额外要求**命令输出本身出现在 guest 侧**，而宿主伪造不了
这一行：整个交互期间宿主 shell 阻塞在 `hyp_vm_run()` 里不读 stdin，宿主自己的
控制台回显也不会出现（实测：本轮送进 guest 的 `echo roundtrip` 那 15 个字节，
日志里 `grep -c roundtrip` = 0，而 `rx_bytes=15` 把它们一个不落地记了账）。

**不能用宿主那条 trap 轨迹当"RBR 读发生了"的证据。** guest trap 的轨迹是**有界**的
——`HYP_TRAP_TRACE_HEAD` = 24（`kernel/hyp/hyp_vcpu.c:1043`），只打前 24 条。本轮
日志里这 24 条**全部**是 guest 打 banner 那一段：trap #0-#2 是 `scause=16`，
即 `HYP_SCAUSE_VIRT_INST`（22，`kernel/hyp/hyp_vcpu.c:73`，`stval` 是指令字
`0x12000073`/`0x18029073`）；#3-#23 是成对的 `scause=15` 读 LSR
（`stval=ffffffc010000005`）与 `scause=17` 写 THR（`stval=ffffffc010000000`）。
**shell 存在的时候轨迹预算早就用完了**，所以"guest 读 RBR"这件事在本门禁里
**永远不会以 trap 行的形式出现**，把判据写成 grep `stval=ffffffc010000000`
会让这条门禁**永远红**。命令输出才是那条真正能判的证据。

**实跑结果（本轮，`make smoke-hyp-shell`）**：**FAIL，如实记红。**

```
smoke-hyp-shell: FAIL forbidden pattern present: ['KERNEL PANIC', '\[PANIC\]']; \
  matching lines from .kernel-build/smoke/hyp-shell-riscv64.log:
  ========== KERNEL PANIC ==========
  [PANIC] arch=riscv64 cpu=0
  [PANIC] task: pid=1 name=kthread state=2
  ...
  [PANIC] attempting firmware poweroff
```

逐条对账（用 `python3` 直接拿 `smoke.py` 的 `grep_matches()` 跑这份日志）：

| 断言 | 结果 |
| --- | --- |
| `HYPVM: guest=/bin/boot/guest-kernel-ramfs\.elf …` | **过**（镜像选对了） |
| `(?:^\|# )AAAABBBBCCCC\r?$`（命令输出） | **缺**（`grep -c AAAABBBBCCCC` = 0） |
| `AAAABBBBCCCC\r?\n# `（回到提示符） | **缺** |
| `(?:^\|# )DDEEEEEEFFFF\r?$`（第二条命令） | **缺** |
| `\[init\] shutting down` | **缺**——guest 根本没走到 mksh，`exit` 那一步从未被 guest 执行 |
| `HYPVM: PASS … rx_bytes=[1-9][0-9]* exit=1\(shutdown\)` | **过**（`rx_bytes=18`） |
| `System is going down for power-off NOW` | **缺**（300 s 超时掐的） |
| forbid `KERNEL PANIC` / `\[PANIC\]` | **违反** |

**这个红说明的正是它该说明的事**：泵是通的（18 个字节全部进了 guest 的环），
但 guest 在 `[INIT] entering scheduler...` 上死于自己的内核栈页 use-after-free
（§3.3 第四次），**从来没有 exec `/bin/init`、从来没有 mksh、从来没有读过一个字节**。
**通道没坏，guest 自己没走到 shell。** 这与上一轮 `smoke-hyp-console` 那个
"`rx_bytes` 绿了但什么都没证明"是同一类结论、但更严格：**这一轮连 guest 走到
`[init] forking...` 之前都没到**。

> **注意 forbid 纪律在这条上是刻意与 `smoke-hyp-console` 不同的。**
> `smoke-hyp-a20os` / `smoke-hyp-console` 之所以**不** forbid `PANIC`，是因为一个
> 没有 rootfs 的 guest **预期**就会 panic，而且它的 panic 字节与宿主的**逐字节相同**，
> forbid 会让一次正确的运行变红。`smoke-hyp-shell` 的对象必须走到 shell，
> 所以两侧任何 panic 都是失败——正因为两侧字节相同，宿主和 guest 的 panic 都在
> forbid 里。
>
> **判据的形状本轮也复核过**：`(?:^|# )TOKEN\r?$` 这个写法同时吃两种 guest tty
> 回显行为（有回显时 token 独占一行、跟在 `# echo …` 那行后面；无回显时 token 粘在
> `# ` 后面），同时**拒绝**宿主的回显（那里 token 前面是 `echo `，不是行首也不是
> `# `）。用离线合成的两份日志分别验过：两种回显形态都全绿，而只有宿主回显、
> guest 没执行任何命令的日志缺 4 条断言。**这不替代真跑**——真跑就是上面那个红。

---

## 4. 当前限制与红线

### 4.1 契约不改

`kernel/include/hyp/hyp_vcpu.h` 是**冻结契约**（文件头逐字说明了：里面的布局是
承重件，改字段名或顺序是跨三个工作包的 ABI 断裂，不是重构）。本文只描述怎么用
它，不建议改它。发现它有问题，走一次显式的契约变更，不要各包自己分叉。

### 4.2 单 guest、单 vcpu、单 hart

- **全局只有一个 guest 槽**。`hyp_vcpu_run()` 用
  `spin_trylock_irqsave()` 抢，抢不到就回 `-EBUSY`（`hyp_vcpu.c:1181-1184`），
  **不等**。两个 `hypvm` 同时跑，第二个立刻拿到 EBUSY 而不是排队。
- **单 vcpu、单 hart，hartid 只能是 0**。`hyp_vcpu_set_boot(vcpu, 0, dtb)`
  不是默认值而是 guest 入口代码需要的值：`entry.S` 在 `a0 == 0` 时直接跳过 SBI
  HSM 的 lottery 交接（`entry.S:44`，`hypvm.c:335-338`）。`a0 != 0` 会走
  secondary 分支，那里要 `sbi_hart_start()` 与 `riscv64_smp_release`，两者都不
  在 v2 的 SBI 面里。
- **多 vcpu 与中断虚拟化**在 [02-roadmap.md](02-roadmap.md) §2，未落地。

### 4.3 无 guest virtio；"进 guest shell 需要先做 virtio"这句话要拆开看

设备模型是**寄存器级**的 16550 UART + CLINT mtime，其余 RAZ/WI，没有 virtio-blk。

**先纠正一个流传的说法：guest 的 virtio-blk 是内核内驱动，不是用户态 `.a20drv`。**
`kernel/drvmod/examples/virtio_blk.c` 只是 16 行的注册壳——`#include` 了
`kernel/drivers/block/virtio_blk.c`（那 1154 行的实现）并把
`A20_DRIVER_PLACEMENT_KERNEL_MODULE` 声明出来（`components/drivers.toml:20-24` 把
它登记成 early 包）。也就是说：**guest 侧驱动 virtio 的代码，与宿主侧是同一份
同一个实现**，只是被 drvmod 包了一层以便从 `/boot/drivers` 加载
（`kernel/arch/riscv64/platform/early_drivers.c:33`）。

**然后是"块设备是不是硬前提"这个问题。答案是分情况的，取决于镜像怎么构建：**

| guest 镜像的构建 | guest 有没有 `/bin/init` | 要进 shell 需不需要块设备 |
| --- | --- | --- |
| `RAMFS_USER=1` | 有。`/bin/init`、`/bin/mksh`、`/bin/help`、`/bin/ls`、`/bin/cat`、`/bin/ps`、`/bin/sleep`、`/bin/timer_preempt`、`/bin/timer_idle` 被 objcopy 成内核数据，由 `kernel/fs/rootfs_user.c:31-41` 的 `g_rootfs_user_overlay` 铺进根 ramfs（程序清单在 `Makefile:1238`），`ramfs_populate_overlay()` 顺带把 `/bin/sh` 做成指向 `/bin/mksh` 的符号链接（`kernel/fs/diskfs/ramfs.c:1285-1301`） | **不需要。** `init_kthread()` 的默认 init 路径就是 `/bin/init`（`kernel/main.c:352`），它会在根 ramfs 上命中 |
| `RAMFS_USER=0`（默认值，`Makefile:111`） | 没有。根 ramfs 里没有 init | 需要——否则 `init_kthread()` 在两处 `vfs_open()` 都失败后 `panic("init: no init program found")`（`kernel/main.c:359-361`） |

`smoke-hyp-a20os` / `smoke-hyp-vm` / `smoke-hyp-vm-96` 三条门禁构建时只传
`ARCH=riscv64 ABI=linux BRINGUP=0`（`tools/smoke_cases.py:614`），没有 `RAMFS_USER=1`，
所以**门禁里那个 guest 是 `RAMFS_USER=0` 镜像**，"挂不上 rootfs 会 panic 在
`init_kthread`"对它们成立。

**所以"必须先实现 virtio 设备模型才能进 guest shell"是不准确的。** 准确的说法是：
块设备只在 `RAMFS_USER=0` 的 guest 上才是硬前提；换成 `RAMFS_USER=1` 的 guest，
块设备可以推迟。真正卡住的是别的东西——guest 要活过它自己的早期启动，而那条路上
现在的拦路虎是设备模型（PLIC 无模型，RAZ/WI）与二级缺页上的访存指令解码，不是磁盘。

**已构建、已跑过、但没走到 shell。** `RAMFS_USER=1` 的 guest 内核由
`tools/targets-images.mk` 的 `$(GUEST_KERNEL_RAMFS_STAMP)` 规则构建，以
`/boot/guest-kernel-ramfs.elf` 打进 FAT32 镜像；门禁 `smoke-hyp-console` 就是 boot
它，日志第 227 行 `HYPVM: guest=/bin/boot/guest-kernel-ramfs.elf elf_bytes=5959752`。
**注意这条规则只在 `ARCH=riscv64 BOARD=qemu-virt-riscv64 BRINGUP=0 RAMFS_USER=0
CONFIG_SLAB_DEBUG=0` 时接进 `$(FAT32_IMG)` 的依赖**（同文件那三个 `ifeq`），
`make dev-build` 走的是 `both-dev` 变体（`Makefile:396` 的 `BUILD_VARIANT`），
**不经过这条规则**——所以镜像里那份是门禁构建时产出的，不是 `dev-build` 的副产物。

**结果是它没有比 `RAMFS_USER=0` 走得更远**：同一个 `[RAMFS] Initialized`（第 556 行）、
同一个 `[SLAB BUG] kfree`（第 557-559 行），同样死在进 shell 之前（§3.3）。

所以上面那张表的第二行现在有两条证据而不是零条：**"不挂块设备也能起来"**由代码
推出并由实测支持（它确实越过了 `init_kthread` 的 panic 点，走到了 RAMFS 之后的
`kfree`），而**"能走到 shell"仍然没有观测到**——拦路的是 guest 自己那个 `kfree`，
不是磁盘。这两句不要合成一句。

真 rootfs（要读镜像上的文件、要能存东西）仍然要 virtio，见
`02-roadmap.md` §4。顺带记一条对那个切片有用的事实：virtio-blk 的完成路径有
**纯轮询**一条，`virtio_blk_wait_req()` 在 `!inst->irq_registered` 时直接抽 used ring
并 `continue`（`kernel/drivers/block/virtio_blk.c:749-763`），所以**中断注入不是块
设备能工作的硬前提**。

### 4.4 GPA < 512 GiB

`hgatp` 是 Sv39x4，但实现只编程根表 `[0, 512)` 的条目（`kernel/include/hyp/hyp.h:26-30`，
`kernel/hyp/hyp.c:20` 的 `HYP_GPA_LIMIT = 1ULL << 39`）。`-g` 超过这个界限的
窗口会被内核拒绝而不是静默失效（`hyp_vm_create` 与 `hyp_vm_set_ram` 都检查，
`hyp.c:90,366`）。解除需要 2048 条目的 x4 根表 + 更宽的根元数据块 +
非叶 PTE 的分段 PPN 编码，是 `pt_meta_t` 的**格式变更**，`02-roadmap.md` §5
按纪律刻意缓行。

### 4.5 中断：宿主 IRQ 即时返回，guest 的 tick 会来，外部中断不会来

三件事，说混一件就会得出错的结论，所以分开写。**本节此前写的是"guest 的时钟
中断与外部中断不会到来"，前半句是错的**，已在下面按代码与 QEMU 源码更正。

**(a) 宿主 IRQ 打在 guest 上即时返回。** `hideleg` 委托了 VS 级中断位，宿主中断在
guest 执行期间照样陷到 HS、走宿主自己的 IRQ 机器，然后 `sret` 直接回 guest
（`kernel/hyp/hyp_vcpu.c:1074-1077`：`if (scause & HYP_SCAUSE_INTR_BIT) return 1;`）。
这是**设计如此**：guest 运行期间的宿主 IRQ 不需要为 guest 保存任何东西。

**(b) guest 的时钟中断会来。** 委托位与使能位都在位，而且到点置位是 QEMU 的行为、
不是软件注入：

- `csrw hideleg` 写的是契约的 `HYP_HIDELEG_DEFAULT`，即 bit 2/6/10 =
  VSSIP/VSTIP/VSEIP（`kernel/include/hyp/hyp_vcpu.h:226-227`；写的地方
  `kernel/arch/riscv64/hyp/hyp_arch.c:284`，在 `hyp_arch_vcpu_setup()` 里）。
- `hstatus.VIE`（bit 10）在 `HYP_HSTATUS_GUEST_ON` 里，随 `hyp_arch_vcpu_setup()`
  与每次 trap 返回重新置上（`hyp_arch.c:151,166-167,315` 与
  `kernel/arch/riscv64/hyp/hyp_vcpu_asm.S:52-53`）。
- vstimecmp 到期时 QEMU 的 `riscv_vstimer_cb()` 置 `env->vstime_irq` 并调
  `riscv_cpu_update_mip()`（QEMU 10.0.13 `target/riscv/time_helper.c:25-30`）；
  写 `vstimecmp`（CSR 0x24d）就是把这个回调挂上去
  （`target/riscv/csr.c:1680-1692` → `riscv_timer_write_timecmp(..., MIP_VSTIP)`）。
- V=1 时 HS 侧的 `hsie` 在 QEMU 里恒为 1（`target/riscv/cpu_helper.c:585`），
  `pending & mideleg & hideleg` 的那几位被硬件重编号后交给 VS-mode
  （`target/riscv/cpu_helper.c:617-634`）。

**两条进 guest 的入口本树都接上了**，取决于 guest 用哪条：

| guest 的时器选择 | 谁写 `vstimecmp` | 依据 | 实测走到过哪一条 |
| --- | --- | --- | --- |
| `riscv,isa` 含 `sstc` | guest 自己 `csrw 0x24d`，被 VTVM 陷阱后由 HS 仿真 | `hyp_vcpu.c:988-1003`（只接受 set/回读，set/clear 形式被拒而不是被近似） | **没观察到走过** |
| `riscv,isa` 不含 `sstc` → 走 SBI legacy `a7=0x00 set_timer` | 分派器代它写同一个 CSR | `hyp_vcpu.c:386-388` | **本轮树上观察到的是这条** |

**当前树走的是第二条**，本轮日志里读得到 guest 自己那行：
`hyp-a20os-riscv64.log` 第 266 行 `[TIMER] backend=sbi freq=10000000 Hz`，
第 263 行 `[FDT] memory node parse failed, using board window 0x80000000..0x480000000`，
第 281 行 `[FDT] no bootargs extracted`。三行都是 guest 的（宿主自己那行
`[TIMER] backend=sstc` 在第 76 行，早于 guest 启动，第 66 行是宿主 banner，
**不能拿它当 guest 的证据**）。

合成 DTB 的 `riscv,isa` 写的是 `rv64imafdch_sstc`（`user/cmds/core/hyp/hyp_guest.c`），
但 guest 侧 `riscv64_fdt_has_isa_extension("sstc")` 返回了 0
（`kernel/arch/riscv64/platform/timer.c`）。**待查**：同一次运行里 guest 的另外两处
FDT 解析也失败了（上面那两行），三处症状指向同一件事：**guest 侧没能解析这份合成
FDT 的 `chosen` / `memory` / `cpus` 三个节点**，本片没有查到根因。

无论走哪条，装定时器这条路都是通的：legacy 那条由分派器代写 `vstimecmp`
（`hyp_vcpu.c:386-388`），Sstc 那条由虚拟指令仿真写（`hyp_vcpu.c:988-1003`），
guest 随后活着走完了 `[INIT] Timer initialized`、`[INIT] Timekeeping initialized`、
`[MM] mm_init`。**这证明"装定时器"是通的**；它**不**证明"定时器中断真的被 guest
处理了"——那要等 guest 活到有一个 tick 可观察的地方。**本片至今没有拿到那个
证据**，不要因为 §4.5 的标题写了"guest 的 tick 会来"就当成已经观测到了。

**(c) 缺的只是外部中断注入。** 全树没有任何一处**代码**写 `hvip`（CSR 0x646）或
`hgeip` / `hgeie`——只有注释提到这两个名字：

```
$ cd /home/fqwqf/OS/A20OS-virt && grep -rn 'hvip\|hgeip\|hgeie' kernel/ user/cmds \
      --include=*.c --include=*.h --include=*.S
kernel/hyp/hyp_dev.c:200: * two-part change -- hvip injection AND a PLIC the guest can SCLAIM from -- and
kernel/include/hyp/hyp_vcpu.h:266: * THERE IS NO INTERRUPT BEHIND THIS.  Nothing here raises hvip or vsip, and
kernel/include/hyp/hyp_vcpu.h:269: * change -- hvip injection PLUS a PLIC the guest can SCLAIM a source from --
```

三处命中全是注释（这就是上一版这里写 `rc=1` 的原因——那时树里还没有这三行注释）。
**没有一行是可执行代码**。

配套的模型侧也没有：PLIC 不在设备模型的表里，非 UART/CLINT 页一律 RAZ/WI 并按页
计数（`kernel/hyp/hyp_dev.c:472-477`），UART 的 IIR 恒回"无中断待处理"
（`hyp_dev.c:71-76,286`），所以 guest 在 `arch_uart_init()` 里打开的 UART 中断源
10 永远不会到来（`01-a20os-guest.md` §6.1）。

**后果**：依赖调度推进的 guest 行为现在做不了，原因是**没有外部中断**，不是没有
tick。反过来说也成立：一条"guest 的定时器中断不来"的判断，在今天的树上多半是在
测别的东西——guest 停在某个地址不动时，先看 `htval` 落在哪（见 §3.3 与 §5.1），
而不是先怀疑委托。

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
| `running` 之后**过一会儿**才出 `exit=` 行 | guest 极慢是**正常**的——每个 console 字节、每次页表走查、每次二级缺页都要陷回宿主（`smoke_cases.py:618-621` 就是为此把 timeout 提到 300 s）。**已验证**：本片的 `hypvm` 两次运行都在这个量级上正常收尾 | 等。门禁的 300 s 是按"guest 比宿主自举慢好几个数量级"定的 |
| 看到 `exit=2(fault)` 就以为失败 | **这是设计内行为**，见 §3.3 | 只看 `marker_seen` 与 `HYPVM: PASS`。`exit=2` 只说明 guest 停在哪儿 |
| 看到 `exit=1(shutdown)` 就以为 guest 跑完了 | **不等于**。`shutdown` 只说明 guest 发了一次 SRST ecall（`hyp_vcpu.c:413-418`），本轮那次是 guest panic 之后发的 | 往上找最后一条 `[PANIC]` / `[SLAB BUG]` / `KERNEL PANIC`。本轮的教训见 §3.3 |
| guest 的日志里出现 `[SLAB BUG] …` 或 `KERNEL PANIC` | **guest 自己**报的，不是 hypervisor 打的。hypervisor 侧的错一律带 `[ERR] hyp:` 或 `hyp:` 前缀，两边不会混 | 归 guest 侧。本轮 guest 在 `[RAMFS] Initialized` 之后死在 `kfree`，根因本片没有定位，也没有证据说与 hypervisor 有关（§3.3） |
| `[PANIC] attempting firmware poweroff` | guest 的 panic 路径在打完堆栈后用 SBI SRST 关机。hypervisor 把这条 ecall 收成 `HYP_EXIT_SHUTDOWN`（`hyp_vcpu.c:413-418`） | 正常机制，被误读成"干净关机"才要紧。判据见上一行 |
| `[ERR] hyp: undecodable guest access at pc=… (insn=…)` | 二级缺页上的访存指令解不出来。行尾的 ` rvc` 表示这条是 16 位编码；没带后缀就是 32 位编码也解不出来（`hyp_vcpu.c:826-835`） | 看 `insn=` 的值。本轮之前这条行**不带机器码**，且当时解码器只认 32 位编码，guest 因此死在一条 `c.sw` 上；RVC 解码器已补上（`hyp_vcpu.c:611-714`），所以这行现在少见多了。仍出现也不影响 PASS 判据 |
| `[ERR] hyp: trap #N scause=… pc=… stval=… htval=…` | 宿主侧的 trap 轨迹。开头若干条 `scause=15/16/17` 是启动头几页按需填充时的正常回访 | 正常诊断输出。真正决定 guest 停在哪的是最后那条 |
| `hyp: guest MMIO at 0x… xN times (no model)` | guest 在探测没有模型的设备区，每 4096 次打一行（`hyp_dev.c` 的 `HYP_RAZ_LOG_EVERY`） | 正常诊断输出，不是错误 |
| `hyp: trap loop at pc=… scause=… (N traps so far)` | 同一处反复陷（`hyp_vcpu.c:1101-1113`） | 与设备模型表对照，一般是某个自旋等一个永远不来的值。契约里最该担心的一个值是 16550 的 LSR bit `0x20`：它必须常置，否则 `arch_uart_putc()` 的自旋把 guest 焊死（`01-a20os-guest.md` §6.1） |
| `[ERR] hyp: undelegated guest page fault scause=…` | guest 自己的 stage-1 缺页没被委托到 VS，落到了 HS（`hyp_vcpu.c:1136-1146`） | 委托掩码没生效，**报 bug** |
| `[ERR] hyp: guest virtual instruction fault not emulated (pc=… stval=…)` | guest 在 VS-mode 做了模拟器不认的虚拟指令（`hyp_vcpu.c:1126-1134`） | 已知限制（`01-a20os-guest.md` §2.3 代价清单的最后一条） |
| `LOCK-STALL` 或 `MCS DEADLOCK` | **并发问题**。单 guest 槽是全局的，锁序有问题 | **报 bug**。带上完整日志。这两个串在 `smoke-hyp-a20os` 的 `forbid` 里（`smoke_cases.py:633`），门禁见到就红 |
| 第二个 `hypvm` 立刻失败（`FAIL vcpu_run`） | 单 guest 槽被占，`hyp_vcpu_run()` 抢锁失败回 `-EBUSY`（`hyp_vcpu.c:1181-1184`） | 等第一个结束。这是设计，不是 bug |
| `smoke-hyp-console-p0` 红：`HYP_VCPU_TEST: FAIL exit=2 want=1 (shutdown)` | guest 回显完那三个字节之后跑飞了：`trap #1 scause=14 pc=0` 之后是 `hyp: guest instruction fetch fault gpa=0 pc=0 rc=-14` | **先看回显那行 `HYP` 在不在**——它在，通道就是通的，坏的是往 guest 镜像里拷的东西。这一条本片踩过：`hyp_test.c` 的 `memcpy(..., sizeof(x)/sizeof(x[0]))` 把元素个数当字节数，拷了 13 字节，于是 `andi` 变成 `0x00000093`（`addi ra,x0,0`），guest 带着过期的 LSR 跳进零填充。**解码器报的 `insn` 全程是对的**，所以别去查解码；把 guest 内存按页倒出来看 `code[i]` 就一眼看见。详见 §3.4.1 |
| `smoke-hyp-console` 红：`rx_bytes=0` | 宿主那行输入没进 guest 的 ingress ring | `rx_bytes` 只在 `hyp_dev_pump_rx()` 里、只在 `uart_try_getc()` 真拿到字节时递增，而 pump 只在 **guest trap** 上跑。若 guest 一个 trap 都没产生（或那行输入在 `hypvm` 起跑**之前**就被 shell 吃掉了），它就停在 0。本门禁用 `sendline_seq` 把输入锚在 `HYPVM: running` 上就是为了这个：hypvm 阻塞在 run loop 里，shell 那时没在读 stdin，提前打的东西已经被 shell 消费掉了 |
| 门禁 timeout（300 s 用尽） | guest 太慢，或卡在某个自旋 | 看日志最后停在哪；对照本表与 `01-a20os-guest.md` §6 |
| `smoke-hyp-shell` 红：`rx_bytes` 过了但两个 token 都没出现 | **泵是通的，guest 没走到 shell**。这是本轮的实际状态（`rx_bytes=18`，token 一次没出现） | 先找 guest 的终点再谈通道：读 `KERNEL PANIC` 之前那几行。本轮是 `[INIT] entering scheduler...` 之后的内核栈页 use-after-free（`[TRAP] pfn ... sits on buddy free list`），**不是**上一轮的 `[SLAB BUG] kfree`。通道那一侧另有 `smoke-hyp-console-p0` 独立守着，它是绿的 |
| `smoke-hyp-shell` 红：缺 `[init] shutting down`，`HYPVM:` 行的 `exit=` 是 `2(fault)` | guest 是 panic 死的，不是被 `exit` 正常关掉的 | 同上：先定位 guest 的 panic 点 |
| `smoke-hyp-shell` 红：token 出现了但缺 `System is going down for power-off NOW` | 交互本身通了，收尾那一下没赶上——`HYPVM: PASS` 之后要给宿主 shell 留出读 `poweroff` 的时间 | 检查 `sendline_seq` 最后一步的 marker 是不是挂在 `HYPVM: PASS` 上（挂在提示符上会把 `poweroff` 偷进已经不存在的环，见 §3.4 的 `poweroff` 那条限制） |

### 5.2 日志在哪

- 门禁：`.kernel-build/smoke/hyp-a20os-riscv64.log`（`smoke_cases.py:615`）。
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