# Milk-V Duo（SophGo CV1800B）

`ARCH=riscv64 BOARD=milk-v-duo`

约 10–13 美元。与 [LicheeRV Nano](licheerv-nano.md) 同属 SophGo IP 家族、同一条启动链，但有一个决定性
差别：**这块板只有 64 MiB RAM**，不是 256 MiB。这使它成为第二目标，而不是第一目标。

**只验证到构建。未在硬件上运行。** 该构建结论现在由 `make check-lao64-board-builds` 在每次提交上复现，
因此这块板不会悄悄编译不过。

## 硬件事实

| 项 | 取值 | 来源 |
|---|---|---|
| SoC | SophGo CV1800B | `cv1800b.dtsi` |
| CPU | T-Head C906 @1 GHz + C906 @700 MHz + 8051 | Sipeed wiki |
| ISA | `rv64imafdc`，`mmu-type riscv,sv39` | `cv180x-cpus.dtsi` |
| RAM | **64 MiB** 集成 DDR2，位于 `0x80000000`，长度 `0x4000000` | `cv1800b.dtsi` `memory@80000000` |
| Timebase | 25 MHz | `cv180x-cpus.dtsi` |
| UART0 | `0x04140000`，IRQ 44 | `cv180x.dtsi` `serial@4140000` |
| MAC | `0x04070000`，IRQ 31，`"snps,dwmac-3.70a"`，内部 PHY | `cv180x.dtsi` `ethernet@4070000` |
| SD | `0x04310000`，IRQ 36，`sophgo,cv1800b-dwcmshc` | `cv180x.dtsi` `mmc@4310000` |
| eMMC | `0x04300000`，IRQ 34，`sophgo,cv1800b-dwcmshc` | `cv181x.dtsi` `emmc@4300000` |
| PLIC | `0x70000000`，64 MiB，101 个中断 | `cv1800b.dtsi` |
| CLINT | `0x74000000` | `cv1800b.dtsi` |
| 启动介质 | TF 卡或 SD-NAND | Sipeed wiki |

外设与 LicheeRV Nano 相同，因为两者都继承 `cv180x.dtsi`；只有 RAM 大小与顶层 compatible 不同。U-Boot
自己的启动日志在本板报 `DRAM: 63.3 MiB`，与 DT 的 64 MiB 节点一致，印证了这个窗口。

## RAM 预算是这里的一切

内核二进制约 2.1–3.8 MiB，视档位而定。放不下的是其余部分：页缓存、lwIP 栈、VFS，以及一个 apk 用户态。

在 64 MiB 上：

- 必须使用 `BRINGUP=1` 与 `RAMFS_USER=1`。
- 必须关闭 swap。`Makefile:187` 已经对没有 swap PTE 编码的架构拒绝 `CONFIG_SWAP`，而 riscv64 有该编码，
  因此要显式传 `CONFIG_SWAP=0`。
- 完整的 `image-world` rootfs 要么构建不出来，要么挂不上。不要在这块板上尝试
  `tools/a20 package milk-v-duo -- PKG_WORLD=base`。

```sh
tools/a20 package milk-v-duo -- RAMFS_USER=1 CONFIG_SWAP=0
```

如果目标是"能启动 A20OS 的最便宜板子"，买 LicheeRV Nano：同为 10–14 美元档，四倍 RAM，驱动完全相同。
只有当 64 MiB 这个限制本身就是需求时，才选 Duo。

## 启动链

```text
CV1800B boot ROM  →  厂商 FSBL  →  OpenSBI  →  kernel
```

与 LicheeRV Nano 相同。mainline U-Boot 有 `configs/milkv_duo_defconfig`，带 `CONFIG_CLK_SOPHGO_CV1800B`、
`CONFIG_MMC` 与 `CONFIG_CMD_MMC`；上游 Linux 带 `arch/riscv/boot/dts/sophgo/cv1800b-milkv-duo.dts`。2026 年
7 月的社区工作正在把该板切到 `CONFIG_OF_UPSTREAM` 并使用上游 SophGo DT，因此 A20OS 最终看到的 DT 会是
上游那份。

那个 defconfig 里的 `CONFIG_SYS_LOAD_ADDR=0x80080000` 与 `CONFIG_CUSTOM_SYS_INIT_SP_ADDR=0x82300000`
描述的是 U-Boot 自己的暂存地址，不是内核装载地址。**本板的内核装载地址未验证**——见下文。

## 部署

部署工具是 `tools/a20`。这里没有 make 命令，这是刻意的：本板没有块驱动，所以没有卡可写，也没有东西让
`dd` 去写镜像。产物是内核，加上把内核交给板上已有启动链的那几条命令。

### 你会得到什么

```sh
tools/a20 package milk-v-duo
```

写出 `build/milk-v-duo/handoff/`：`kernel.bin`、`uboot.cmd`（在 U-Boot 提示符下逐行执行的命令），以及一份
携带同样说明的 `README.md`。

装载地址**不**写在本文件或实例里。`a20 package` 从刚构建出的内核的第一个 `PT_LOAD` 头里读出它，因此链接
脚本会重定位镜像的板子不会在这里留下过期地址。

刻意不提供 device tree。U-Boot 在 `a1` 里传入它自己的板级 DTB，而 `riscv64_memory_init()` 与平台
device tree walker 读的正是那份 DTB。再从内核树里带第二份拷贝，就多了一个必须与固件保持同步的东西，
而它漂移时没有任何机制会察觉。

### 验证

`tools/a20 deploy milk-v-duo` 完成构建、打包、打印 handoff 命令，然后接上控制台检查结果。只想观察一块
已在运行的板子：

```sh
tools/a20 console milk-v-duo
```

### 把它弄到板上

1. 按板子自己的方式启动：厂商 FSBL，然后 OpenSBI，然后 U-Boot。把 FSBL 留在卡上——卡是唯一的退路。
2. 打断 U-Boot 的自动启动。
3. 告诉 U-Boot 主机在哪，然后装载内核：

   ```text
   setenv serverip <你的主机 IP>
   setenv ipaddr <板子 IP>
   tftp <loadaddr> kernel.bin
   ```

   从 `build/milk-v-duo/handoff/` 取 `kernel.bin`，把 `<loadaddr>` 换成 `uboot.cmd` 打印出的地址。传输
   方式随你的主机怎么连到板子而定——上面的 TFTP，或者 U 盘加
   `fatload usb 0:1 <loadaddr> kernel.bin`。
4. 启动内核：

   ```text
   booti <loadaddr> - <loadaddr>
   ```

   中间的 `-` 是 device tree 指针。留空是正确的：U-Boot 自己会在 `a1` 里传入它自己的板级 DTB。

控制台是 `/dev/ttyUSB0`，115200。若适配器枚举成别的节点，改掉它——插拔前后各执行一次 `ls /dev/ttyUSB*`。

只有出现下面两行才算启动成功：

| 行 | 证明什么 |
|---|---|
| `[FDT] RAM range ...` | 固件交出了 device tree |
| `System ready` | 内核进入了用户态 |

实例里的 `console_check` 等的正是这两行，因此 `a20 console` 能区分"启动"与"卡住"。随后它执行
`cat /etc/os-release` 与 `poweroff`，并检查 `A20OS` 与 `poweroff`。

### RAM 预算在这里压倒一切

64 MiB 是整个设计的约束，也决定了 bring-up 路径为什么是这样。在 MMU 与 NOMMU 构建之间做选择之前，
先读[RAM 预算是这里的一切](#ram-预算是这里的一切)，并优先使用：

```sh
tools/a20 package milk-v-duo -- NOMMU=1 RAMFS_USER=1
```

NOMMU 构建跑不了 `mksh`，也跑不了任何需要 `fork`/`mmap` 的东西，因此在这块板上，控制台内检查就是
实际上限，而不是进一步使用的起点。

### 如果启动不了

1. **控制台什么都没有。** UART 或 pinmux 错了；UART 时钟由 FSBL 释放，所以静默通常意味着 FSBL 没跑。
2. **U-Boot banner 之后静默。** 装载地址——本板最可能的失败点。见下一节。
3. **`[FDT] memory node unavailable`。** U-Boot 没有传 `a1`。
4. **RAM 范围不对。** U-Boot 的 DTB 与板子不一致；那是固件问题，不是内核问题。

## 未验证：内核装载地址

A20OS 的 riscv64 `KERNEL_ENTRY` 与 `PHYS_BASE` 是编译期常量，架构默认的 `0x80200000` 是 QEMU virt 用的
地址。U-Boot 遵循 `kernel_addr`，或启动脚本/bootflow 给出的 `load` 地址，它不一定等于 `0x80200000`。

如果内核在第一次 `printf` 之前就卡住，这是第一个要查的点：把 U-Boot 的 `kernel_addr` 与 `KERNEL_ENTRY`
对一下；若不同，就**一起**改板级 `<board>_platform.h` 里的 `KERNEL_ENTRY` 与板级 `ldscript.ld` 里的
`PHYS_BASE` / `VIRT_BASE` 对。两者必须一致，否则内核会跳到一个它并未按之链接的地址。

这里把它标为未验证，是因为没有板子就无法定论。

## 复用，以及缺什么

与 LicheeRV Nano 相同：

- 共享 `kernel/drivers/irqchip/plic.c`——DTS 写 `thead,c900-plic`，mainline 把它绑定到标准 SiFive PLIC 驱动
- 不需要覆盖链接脚本；DRAM 在 `0x80000000`
- 不需要架构适配；`rv64imafdc` 与 riscv64 构建一致
- 时基与 `poweroff`/`reboot` 走 SBI

缺的，两者都需要真正的驱动：

- **存储。** `sophgo,cv1800b-dwcmshc` 在 `0x04310000`（SD）与 `0x04300000`（eMMC，见于 Duo 256M 与 Duo S）。
  A20OS 的 `dw_sdio.c` 覆盖的是较老的 DesignWare SDIO/MMC MSHC 布局，不是 DWCMSH。
- **以太网。** `snps,dwmac-3.70a` 加内部 PHY。基础版 Duo 还需要**外部 1:1 变压隔离器与 RJ45**，而板子没有
  焊——不少卖家出售的 64 MiB 版本缺这两样，所以本板上"没有网络"的报告，原因可能是缺变压器而不是驱动
  bug。
- **SMP。** 上游没有描述，因此 `smp` 为 `NULL`。Duo S 的第二个核由 Sipeed 文档而非 DT 记录。

## 在宣称硬件验证之前

与 LicheeRV Nano 同样的五步，只是把装载地址检查提到最前面，因为它是本板最可能的失败点。把厂商 FSBL
留在卡上。
