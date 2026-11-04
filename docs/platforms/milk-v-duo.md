# Milk-V Duo / CV1800B 64 MiB

目标为原版 **CV1800B、64 MiB RAM** 的 Milk-V Duo，构建选择`ARCH=riscv64 BOARD=milk-v-duo`。Duo 256M、Duo S 使用不同 SoC/内存配置，不能据此认定兼容。

当前提供板级源码和 `milk-v-duo` 构建实例，**没有完成可安全执行的部署路径，也未在真板验收**。现有实例的 DTB 交接命令和 UART 访问方式仍有阻断项，见下文。构建成功、包目录生成或厂商 Linux 能运行，均不表示这份 A20OS 配置已经能上板。

## 配置与能力

| 项目 | 当前配置 / 边界 |
| --- | --- |
| 主执行核 | RV64 C906，S-mode；上游描述 `rv64imafdc`、Sv39 |
| RAM | `0x80000000..0x84000000`，64 MiB；固件/RTOS 保留区须从 DTB 排除 |
| 内核 Image 地址 | `0x80200000`；打包工具从 ELF 的实际 PT_LOAD 读取 |
| 实例 | `BRINGUP=1 NOMMU=1 RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded CONFIG_SWAP=n` |
| UART0 | 物理 `0x04140000`，IRQ 44；硬件需要四字节寄存器步长和 32 位访问 |
| 中断/定时器 | PLIC `0x70000000`，SBI 定时器；DTB 时基优先，回退 25 MHz |
| SD / eMMC | A20OS 尚无匹配 DWCMSH 控制器的驱动；原版板不提供 eMMC 部署能力 |
| Ethernet | A20OS 尚无此板 DWMAC 驱动；部分板还需要外接隔离变压器与 RJ45 |
| SMP | 板级 `.smp = NULL`，不能把另一颗核作为 A20OS SMP CPU |

RAMFS 用户态是构建配置，不是已验收的 shell 能力。不能把完整 Alpine world、 SD 镜像或 LicheeRV Nano 的 SG2002 FIP 直接用到这块 64 MiB CV1800B 板。

## 宿主依赖、构建与产物

下面以 Debian/Ubuntu 为例，Python 需为 3.11 或更新版本：

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential git curl ca-certificates xz-utils file python3 \
  dosfstools e2fsprogs mtools util-linux \
  gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
  gcc-riscv64-linux-gnu libc6-dev-riscv64-cross
```

在已取得的 A20OS 源码根目录更新子模块；以下步骤假定当前目录包含仓库 Makefile：

```sh
git submodule update --init --recursive
```

尚无 rustup 时安装，再为本仓库固定工具链安装目标：

```sh
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/a20-rustup-init.sh
sh /tmp/a20-rustup-init.sh -y --profile minimal --default-toolchain 1.96.0
. "$HOME/.cargo/env"
rustup toolchain install 1.96.0 --profile minimal
rustup target add --toolchain 1.96.0 \
  riscv64imac-unknown-none-elf riscv64gc-unknown-none-elf
```

已有 rustup 可跳过下载安装器，但仍执行 `toolchain install` 和 `target add`。构建使用仓库 `rust-toolchain.toml`，不要求另外安装 nightly。

从仓库根目录执行：

```sh
tools/a20 check instances/milk-v-duo.toml
tools/a20 build milk-v-duo --dry-run
tools/a20 build milk-v-duo
tools/a20 package milk-v-duo
```

`build` 生成内核；`package` 生成 `build/milk-v-duo/handoff/`，包括：

- `kernel.elf`、带 RISC-V Image 头的 `kernel.bin`。
- 从当前实例生成的 `uboot.cmd` 和说明文件。
- 记录装载地址、包含 BSS 的 Image 范围、文件长度和 SHA-256 的 `manifest.json`。

实际编译目录由构建选项决定，可查询：

```sh
tools/a20 show-vars milk-v-duo
tools/a20 ledger milk-v-duo
```

包不是整卡镜像，不包含 CV1800B FSBL、OpenSBI、U-Boot 或匹配 DTB。此板没有 `a20 prepare` 固件目标，也没有自动生成空白卡启动链的目标。`tools/a20` 的额外 Make 参数须放在 `--` 后，不能使用旧文档中的无分隔符写法。

## 上板前尚未满足的条件

目前不能直接执行 `a20 deploy milk-v-duo` 或包内 `uboot.cmd` 作为可用部署流程：

1. 实例中的 `booti ${loadaddr} - ${loadaddr}` 把 Image 地址传给了 DTB 参数。`booti` 的第二参数 `-` 表示没有 initrd；**第三参数必须是独立、有效的 DTB 地址**，不会因为填写 `-` 而自动改用 U-Boot 控制树。
2. CV1800B UART0 是 DW APB UART。当前 `riscv64/include/console.h` 的 32 位步长分支只覆盖 LicheeRV Nano；Duo 仍走字节步长的 16550 分支并重写初始化寄存器。这与此板 UART 硬件协议不匹配，不能把无输出归结为串口线或加载地址。
3. 必须准备能初始化本板 DDR 并提供 S-mode SBI 服务的 **CV1800B** 启动链，明确 DTB、固件/RTOS 保留内存、U-Boot 工作区与整个 Image/BSS 的位置。当前仓库尚未为这个实例提供经核对的完整交接配方。

正确的协议契约是 `booti <Image地址> - <独立DTB地址>`，其中两个范围不得重叠， DTB 必须匹配原版 Duo 并保留固件内存；这只是待实现交接的要求，不是当前可执行配方。厂商镜像具备 USB gadget、SD 或网络功能，也不等于 U-Boot 和 A20OS 都具备同样的传输/驱动能力。

## 串口与后续验收

准备 3.3 V 逻辑 USB-TTL、独立电源、microSD 与读卡器。按[Milk-V 官方接线](https://milkv.io/docs/duo/getting-started/duo)，板 TX 为物理 pin 16，接适配器 RX；板 RX 为 pin 17，接适配器 TX；GND 为 pin 18。 115200、8N1、无流控；不要将适配器 5 V/RS-232 信号接入 UART。

先用厂商启动卡验证接线和 DDR/U-Boot；保留这张可恢复卡。上述阻断修复后，A20OS 验收至少应包括正确的 `[FDT] RAM range`、`System ready`、实际 RAMFS shell 交互以及定时器/中断测试。现有实例的 `console_check` 只匹配宽泛字符串，并随后发送`cat /etc/os-release`、`poweroff`，不能代替完整串口日志和实际功能验证。`System ready` 本身不证明已经进入用户态。

## 故障与恢复

- 厂商卡也没有串口输出：检查供电、TX/RX/GND、115200 和适配器流控。
- 厂商系统可启动、A20OS 无输出：先处理上面的 UART 与 DTB 阻断，不能靠反复写卡排除。
- 内存范围超过 64 MiB，或固件保留区落入分配器：停止试启动，修正匹配 DTB 和保留区处理。
- 试启动失败：断电，使用原厂可启动卡恢复。当前没有可供照抄的 A20OS SPI/SD-NAND 刷写流程。

硬件引脚与厂商卡安装步骤见[Milk-V Duo 官方文档](https://milkv.io/docs/duo/getting-started/duo)。板级地址与内存上限见 `kernel/arch/riscv64/boards/milk-v-duo/`；工具操作见[实例参考](../dev/instances.md)，其他平台见[平台约定](README.md)。
