# Sipeed LicheeRV Nano（SophGo SG2002）

`ARCH=riscv64 BOARD=licheerv-nano`

仓库里最便宜的板子（约 9–14 美元），也是需要新代码最少的一块：核心是 T-Head C906，上游描述为
`rv64imafdc`、`mmu-type riscv,sv39`，正是 riscv64 构建已经瞄准的目标。

**只验证到构建。未在硬件上运行。** 该构建结论现在由 `make check-lao64-board-builds` 在每次提交上复现，
因此这块板不会悄悄编译不过。下文标注"未验证"的内容，都是因为这块板还没有在 A20OS 下上电。

## 硬件事实

全部来自上游，而非厂商文档：

| 项 | 取值 | 来源 |
|---|---|---|
| SoC | SophGo SG2002 | `sg2002.dtsi` |
| CPU | 1× T-Head C906 @1 GHz（另有 C906 @700 MHz，上游未描述） | `cv180x-cpus.dtsi` |
| ISA | `rv64imafdc`，扩展 `i m a f d c zicntr zicsr zifencei zihpm` | `cv180x-cpus.dtsi` |
| MMU | `riscv,sv39` | `cv180x-cpus.dtsi` |
| RAM | 256 MiB 集成 DDR3，位于 `0x80000000`，长度 `0x10000000` | `sg2002.dtsi` `memory@80000000` |
| Timebase | 25 MHz | `cv180x-cpus.dtsi` `timebase-frequency` |
| UART0 | `0x04140000`，256 字节，IRQ 44 | `cv180x.dtsi` `serial@4140000` |
| MAC | `0x04070000`，IRQ 31，`"sophgo,cv1800b-dwmac", "snps,dwmac-3.70a"` | `cv180x.dtsi` `ethernet@4070000` |
| SD | `0x04310000`，IRQ 36，`"sophgo,sg2002-dwcmshc"` | `cv180x.dtsi` `mmc@4310000` |
| PLIC | `0x70000000`，64 MiB，101 个中断 | `sg2002.dtsi` `interrupt-controller@70000000` |
| CLINT | `0x74000000` | `sg2002.dtsi` `timer@74000000` |
| Pin mux | `0x03001000`（sys）、`0x05027000`（rtc） | `sg2002.dtsi` |
| Clock | `0x03002000` | `sg2002.dtsi` |
| 启动介质 | TF 卡，或 TF 槽下的 SD-NAND | Sipeed wiki |

UART0 是 `"snps,dw-apb-uart"`，带 `reg-shift = <2>` 与 `reg-io-width = <4>`：一个 16550 寄存器布局的
DesignWare APB UART，正是各架构 `console.h` 已经声明的形态。

外设中断编号为 `SOC_PERIPHERAL_IRQ(nr) = nr + 16`，因此 DTS 里的中断号比 PLIC 线号小 16。本板使用线号。

## 树上已有的、可直接复用的部分

- **PLIC 是共享实现。** DTS 写的是 `"thead,c900-plic"`，mainline Linux 把它绑定到
  `drivers/irqchip/irq-sifive-plic.c`——标准 SiFive PLIC，不是单独的 T-Head 驱动。因此
  `kernel/drivers/irqchip/plic.c` 原样适用。（mainline 为该 compatible 标了 `PLIC_QUIRK_EDGE_INTERRUPT`，
  因为 C900 可以发出边沿触发中断；本 DTS 里每个中断都是 `IRQ_TYPE_LEVEL_HIGH`，走的是无需特殊处理的
  电平路径。）
- **无需覆盖链接脚本。** DRAM 从 `0x80000000` 开始，正是 `kernel/arch/riscv64/boot/ldscript.ld` 已指向的
  地址，架构默认镜像原样正确。
- **无需架构适配。** ISA 字符串与构建的 `-march=rv64imafdc_zicsr_zifencei -mabi=lp64` 完全一致。
- **时间与电源走 SBI。** riscv64 读 `time` CSR 并通过 `firmware_set_timer()` 编程比较器；没有任何 riscv64
  代码读取 `CLINT_BASE`，因此上面的 CLINT 地址只是记录，并未使用。

## 启动链

```text
CV1800B boot ROM  →  厂商 FSBL（cv181x.bin）  →  OpenSBI  →  kernel
```

FSBL 初始化时钟与 DRAM 并加载 OpenSBI；厂商工具把 FSBL + OpenSBI + U-Boot 打包成 `fip.bin`，放在 TF 卡
的 FAT 分区上。FSBL 是公开发布的，**没有签名**——与 Amlogic 不同，这里没有厂商签名 blob。

mainline U-Boot 有板级目标 `configs/licheerv_nano_defconfig`，启用了 ns16550 UART 与 **Synopsys DesignWare
MSHC** 驱动。这一点值得知道：它同时确认了控制台与 SD 控制器都落在 A20OS 已经会说的 IP 上。

## 部署

部署工具是 `tools/a20`。本节没有 make 命令，这是刻意的：本板没有块驱动，所以没有卡可写，也没有东西
让 `dd` 去写镜像。产物是内核，加上把内核交给板上已有启动链的那几条命令。

### 你会得到什么

```sh
tools/a20 package licheerv-nano
```

写出 `build/licheerv-nano/handoff/`：

| 文件 | 内容 |
|---|---|
| `kernel.bin` | 原始内核，装载到 `uboot.cmd` 里给出的地址 |
| `uboot.cmd` | 在 U-Boot 提示符下逐行执行的命令 |
| `README.md` | 同样的说明，随产物一起提供 |

装载地址**不**写在本文件或实例里。`a20 package` 从刚构建出的内核的第一个 `PT_LOAD` 头里读出它，因此链接
脚本会重定位镜像的板子不会在这里留下过期地址。对本板它解析为 `0x80200000`，即 riscv64 的 `PHYS_BASE`。

刻意不提供 device tree。见下文[为什么不带 DTB](#为什么不带-dtb)。

### 把它弄到板上

1. 按板子自己的方式启动：厂商 FSBL，然后 OpenSBI，然后 U-Boot。FSBL 与 `fip.bin` 打包来自 SophGo 工具；
   把 FSBL 留在卡上，因为卡是唯一的退路。
2. 打断 U-Boot 的自动启动。
3. 告诉 U-Boot 主机在哪，然后装载内核：

   ```text
   setenv serverip <你的主机 IP>
   setenv ipaddr <板子 IP>
   tftp 0x80200000 kernel.bin
   ```

   从 `build/licheerv-nano/handoff/` 取 `kernel.bin`。传输方式随你的主机怎么连到板子而定——上面的 TFTP，
   或者 U 盘加 `fatload usb 0:1 0x80200000 kernel.bin`。
4. 启动内核：

   ```text
   booti 0x80200000 - 0x80200000
   ```

   中间的 `-` 是 device tree 指针。留空是正确的：U-Boot 自己会在 `a1` 里传入它自己的板级 DTB。

### 验证

`tools/a20 deploy licheerv-nano` 完成构建与打包、打印 handoff 命令，然后接上控制台检查结果。只想观察
一块已在运行的板子：

```sh
tools/a20 console licheerv-nano
```

控制台是 `/dev/ttyUSB0`，115200 8N1。若你的适配器枚举成别的节点，改掉它——插拔前后各执行一次
`ls /dev/ttyUSB*`。

只有**同时**出现下面两行才算启动成功：

| 行 | 证明什么 |
|---|---|
| `[FDT] RAM range ...` | 固件交出了 device tree |
| `System ready` | 内核进入了用户态 |

实例里的 `console_check` 等的正是这两行，因此 `a20 console` 能区分"启动"与"卡住"。若你看到的是 RAM 窗口
回退消息而不是一个范围，说明 U-Boot 没有传 `a1`，是装载错了，不是内核的问题。

随后它执行 `cat /etc/os-release` 与 `poweroff`，并检查 `A20OS` 与 `poweroff`。

### NOMMU

riscv64 在 `NOMMU_SUPPORTED_ARCHES` 里，想要更小的占用只差一个覆盖参数：

```sh
tools/a20 package licheerv-nano -- NOMMU=1
```

VisionFive 2 的链接脚本为自己的 DRAM 窗口把镜像重定位到 `0x40200000`；本板 DRAM 在 `0x80000000`，所以
架构默认值成立，不需要 NOMMU 专属脚本。注意 NOMMU 跑不了 `mksh`，也跑不了任何需要 `fork`/`mmap` 的
东西，因此上面的控制台内检查就是 NOMMU 构建的上限。

### 为什么不带 DTB

这一族的板子都经厂商 FSBL 与 U-Boot 启动，U-Boot 在 `a1` 里传入它自己的板级 DTB。那份 DTB 是板子的
权威描述：`riscv64_memory_init()` 从中读 RAM，平台 device tree walker 从中读设备。再从内核树里带第二份
拷贝，就等于多了一个必须与固件保持同步的东西，而树里没有任何机制会在它漂移时察觉。

后果是真实的，值得说清楚：如果你的 U-Boot 编译时没有带 DTB，或传进来的 DTB 描述了不同的 RAM 窗口，
内核会回退到板级编译期窗口，`[FDT] RAM range` 那行就不会出现。

### 如果启动不了

按这个顺序排查，它按实际最先出问题的顺序排列。

1. **控制台什么都没有。** UART 或 pinmux 错了。UART0 在 GPIO 排针上的 `0x04140000`；时钟由 FSBL 释放，
   所以控制台静默通常意味着 FSBL 没跑，而不是内核错了。
2. **U-Boot banner 之后静默。** 装载地址。把 `uboot.cmd` 里的地址与板上 `bdinfo` 的结果对一下。
3. **`[FDT] memory node unavailable`。** U-Boot 没有传 `a1`。检查你的 U-Boot 是否带 `CONFIG_OF_BOARD`
   编译，或 `booti` 是否指向了一份被截断的 DTB。
4. **RAM 范围不对。** U-Boot 的 DTB 与板子不一致。那是固件问题；`riscv64_memory_init()` 只是忠实报告
   它拿到的内容。
5. **`System ready` 之后卡住。** 你已经过了 bring-up。这不再是本文的主题，见 `porting-guide.md`。

## 这个移植做了什么、没做什么

做了：由固件 DTB 收窄的 RAM 发现、共享 PLIC、SBI 时基、UART0 控制台、经 SBI SRST 的 `poweroff`/`reboot`。

没做，且每一项都需要真正的驱动工作而不是往设备表里加一行：

- **存储。** SD 控制器是 DesignWare CMSH（`sophgo,sg2002-dwcmshc`，上游由 `sdhci-of-dwcmshc.c` 驱动）。
  A20OS 的 `dw_sdio.c` 面向 VisionFive 2 的 dw-mci 所用的较老 DesignWare SDIO/MMC MSHC 寄存器布局。
  两者是近亲，但近到不足以假装一个驱动能覆盖两个。这是越不过仅 RAM 启动的那道坎。
- **以太网。** MAC 是 `snps,dwmac-3.70a`，比 `ls2k_gmac.c` 面向的 DWMAC1000 更老。它还有内部 PHY
  （`phy-mode = "internal"`）。只有 `-E` 与 `-WE` 变体才焊出 RJ45。
- **SMP。** 第二个 C906 在上游 device tree 里没有描述，因此 `smp` 为 `NULL`。`porting-guide.md` 明确
  规定：无法启动从核的板子不得提供一个假的空 ops 表。

## 在宣称硬件验证之前

1. `tools/a20 package licheerv-nano` 产出 `build/licheerv-nano/handoff/kernel.bin`。
2. FSBL 起来之后按[部署](#部署)一节通过 U-Boot 装载它。
3. 在 GPIO 排针、115200 8N1 的 UART0 上确认 `System ready`。
4. 确认 DTB 到达了内核——`riscv64_memory_init()` 会打印 `[FDT] RAM range ...`；静默回退到板级窗口
   意味着 U-Boot 没传 `a1`。
5. 按 `porting-guide.md` 记录验收表。在第 3 步发生之前，关于这个移植唯一成立的说法是"它能编译"。

保持恢复路径完整：把厂商 FSBL 留在卡上，不要覆盖它。卡是唯一的退路。
