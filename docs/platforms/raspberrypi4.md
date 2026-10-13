# Raspberry Pi 4：接入公共原生内核与 SMP

2026-10-12。新增 `kernel/platform/raspberrypi4/`，目标为 BCM2711 的
AArch64 原生内核。QEMU `raspi4b` 的内核指令、四核调度、睡眠与 IPI 已有
功能证据；没有树莓派实物、联网集群、完整用户态或 Minecraft 客户端证据。

## 公共机制与板级事实

树莓派沿用 `core/smp.c`、同一任务/PID 表、调度器、亲和性、Park/Wake、
堆分配和退出回收。没有新增树莓派专用的成员、作业或线程协议。

| 层次 | 实现 |
|---|---|
| 公共 CPU 生命周期 | `smp_platform_ops_t`，discover/start/send_ipi |
| 逻辑 CPU 与任务上下文 | AArch64 的 CPU 本地 `TPIDRRO_EL0`；随 CPU 固定，不随线程迁移 |
| 固件发现 | 公共 `core/fdt_view.c` 的有界只读 FDT v17 遍历器 |
| 从核启动 | board 读取 DTB 的 spin-table 描述，清理页表/上下文至 PoC，发布入口并 SEV |
| MMU/异常级别 | 架构入口接受 assembler-safe 启动布局参数；可选 EL3/EL2→EL1 转换 |
| GIC 与串口 | 复用 AArch64 GICv2 适配和 PL011；board 提供地址、GPIO 与波特率参数 |
| 定时器 | 复用 CNTP/CNTPCT，以 `CNTFRQ_EL0` 的实际频率进行时间换算 |
| 集群服务接口 | 沿用公共 CL/服务/作业接口；本端口还没有可用网卡驱动，尚不能做联网验收 |

AArch64 旧代码把 MPIDR 的低八位直接作为逻辑 CPU 号。现在启动核显式设置
逻辑号 0，从核采用公共启动流程传入的逻辑号，C 代码与异常入口读取同一
CPU 本地寄存器。`TPIDR_EL1` 仍用于当前任务，EL0 TLS 仍使用 `TPIDR_EL0`。
硬件 ID 保留 MPIDR 的 Aff0–Aff3；板级负责 GIC 目标与固件 CPU ID 的对应。
测试特意打乱 DTB 顺序，并只启用硬件 CPU0/CPU3，避免靠核号恰好相等通过。

这是一个芯片内硬件一致性执行域。把多块 Pi 或服务器组成**同一个原生内核**，
仍需真实的共享 RAM、相容地址布局、跨板原子性和缓存一致性契约；普通网线
不能提供这些条件。独立内核之间的服务/作业集群使用已有 CL 接口。
跨 ISA 线程直接迁移的约束见 [公共执行域设计](../cluster/27-portable-domains.md)。

## 启动与内存契约

初始 profile 仅支持 MMU、`NR_CPUS=1..4`，内核物理入口 `0x80000`，
高地址别名 `PAGE_OFFSET + PA`。board 链接脚本复用架构的段布局与 W^X 边界。
Image 开头使用标准 AArch64 64 字节可执行头；固件把有效 DTB 的物理地址放在
x0，MMU 关闭，内核镜像可见，各 CPU 已处于同一硬件一致性域。
入口可从 EL1、EL2 或 EL3 归一到 EL1；更高异常级别下配置计时器访问权限。
这不替代固件的 DDR、系统缓存、时钟和 Cortex-A72 一致性初始化。
实物固件必须设置 SMPEN 并把 GIC SGI/PPI/SPI 配置为 EL1 可访问的 Group 1；
对应初始化可见 Raspberry Pi 官方 armstub8。QEMU 的 GIC 模型没有代替这项实物验收。

启动页表只覆盖低 1 GiB Normal RAM 与 `0xc0000000..0xffffffff` Device 窗口。
board 的 `PHYS_MEMORY_END`/`board_config.ram_end` 是映射上限，**不是实际可用 RAM**。
`arch_ram_range()` 读取 board 在早期解析的实际范围，供公共 frame allocator 使用：

1. 验证 BCM2711 compatible、root cell widths、CPU/内存描述。
2. 读取从 0 开始的 RAM 范围，并限制在低 1 GiB。
3. 在 DTB 自身、reserve map 和静态 `/reserved-memory` 区域之前截断可分配前缀。
4. 与内核重叠的保留区、无效范围或不足的低内存明确拒绝。

QEMU 把 DTB 放在 128 MiB，本次普通用例因此只分配 `0..128 MiB`；不是宣称
用满 2 GiB。测试另验证 64 MiB 处的 reserve map 与 reserved-memory 都生效。
这是保守前缀分配，不是完整的多区间内存发现；DTB/固件保留区仍被启动直接映射覆盖，
没有完成 `no-map` 的页表隔离、动态 reservation、CMA 或高地址 RAM 管理。

FDT reader 本身无堆分配，调用者明确给出可读映射大小。它验证总长、结构/字符串
区域、reserve map 终止符、深度上限、属性边界和名称终止，并先验证整棵树再使用
早期记录。Pi 最多接收 2 MiB DTB；数据被复制到板级元数据，后续不依赖 DTB 指针。
旧架构的其他 DTB parser 没有在本阶段全部迁移到新 reader。

固件必须从硬件 CPU0 启动。CPU 节点须为 `spin-table`、硬件 ID 0–3，并带 BCM2711 的 release slot
`0xd8 + 8 * hw_id`；disabled CPU 保持固件等待。发布过的非零 slot 不允许单核
重新启动。上下文和动态页表通过 PoC 清理供尚未开启 cache 的 AP 读取；随后进入
**公共** AArch64 secondary→proc/trap/timer→online→idle 流程。

PL011 地址为 `0xfe201000`，IRQ 为 SPI121+32=153；GICD/GICC 为
`0xff841000/0xff842000`。GPIO14/15 配置 ALT0、关闭 pull，115200 分频假设
固件提供 48 MHz UART clock。计时器不硬编码板频率：本机 QEMU 实测 62.5 MHz。

## 可复现使用

```sh
# 新板模型与标准 Image/DTB handoff；不是 QEMU virt。
tools/a20 check raspberrypi4-qemu raspberrypi4
tools/a20 build raspberrypi4-qemu
tools/a20 run raspberrypi4-qemu

# 功能与拒绝路径，报告包含实际命令、镜像/DTB SHA256。
make -j8 check-raspberrypi4
make check-fdt-view

# 同一公共功能测试在三个服务器 ISA 上执行。
make check-smp-native
```

需要 `aarch64-linux-gnu-gcc`、支持 `raspi4b` 的 `qemu-system-aarch64`。
FDT 边界测试另需宿主 GCC/ASan/UBSan；fixture 是显式编码的标准 DTB，不需要 dtc。
QEMU fixture 的 CPU/RAM 描述不是实物固件的测量结果。

`board.mk` 经公共可选 include 选择 QEMU machine、物理 CPU 数和启动镜像。
QEMU 机器固定为 2 GiB/4 个物理核；内核容量仍可单独设为 1–4。
单核模式使用无自检 bootargs，其余核仍停在固件入口。`kernel-only` 生成的
`qemu-fixture.dtb` **只供模拟器**，不可当作物理树莓派固件 DTB。

`BRINGUP=1` 时可用 `a20.smp_test=1` 开启公共自检。对于既有 ELF handoff 不
提供命令行的平台，`SMP_SELFTEST=1` 使用独立 `-smp-test` 构建目录启用相同
测试；默认关闭，非 bringup 或单核配置会拒绝该构建开关。

## 功能证据与当前限制

QEMU 10.0.13 的普通、乱序、稀疏核、reserve map、reserved-memory 五种有效
配置通过；错误 SoC、release slot、重复 CPU、启动方法、内核/保留区重叠五种
输入在早期拒绝。有效配置必须执行真实线程并证明：

- 同一 PID 从最后一个在线逻辑核迁移到逻辑 CPU0，原栈地址/内容不变；
- 在远端分配、CPU0 修改并释放同一堆对象；
- 公共 Park/Wake、定时 sleep、每个在线核的实际 timer IRQ、远端 IPI 应答；
- worker exit/wait/reap，controller 退出后 task/PID/list 数量回到基线。

报告在 `.kernel-build/cluster/raspberrypi4/`、`smp-native/` 和 `fdt-view/`。
有界 reader 的宿主 ASan/UBSan 用例覆盖 926 个截断、926 个字节变异及七个
无效 header。这里只称边界测试，没有声称覆盖所有恶意/损坏 DTB。

树莓派没有实现电源关闭和 watchdog reboot；firmware hook 当前只屏蔽 IRQ 并
halt。QEMU 因此由测试程序在内核终态输出后 SIGTERM 结束，不能把关机消息
当作电源关闭通过。也尚无 SD/eMMC、GENET/PCIe/USB、图形输入、rootfs/JVM
或多机共享内存适配；QEMU 自身还缺少 GENET 和 PCIe 模型。设备 MMIO 的
loadable-driver 窗口及远端 TLB shootdown 也未在此 profile 完善。

物理 manifest 仅声明串口与启动观察点，实物仍需验证固件 DTB、UART clock、
EL/一致性设置、reserve map、页表属性、timer/IPI 和整域复位。当前不能称为
完整物理树莓派操作系统或 Minecraft 客户端已经完成。

## 事实来源

- [Raspberry Pi BCM2711 外设手册](https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf)：地址、GPIO 与 UART。
- [Raspberry Pi Linux BCM2711 DTS](https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/arch/arm/boot/dts/broadcom/bcm2711.dtsi)：GIC、IRQ、CPU spin-table 描述。
- [AArch64 启动协议](https://docs.kernel.org/arch/arm64/booting.html)：Image 头、x0 DTB、异常级别与一致性条件。
- [QEMU Raspberry Pi 文档](https://www.qemu.org/docs/master/system/arm/raspi.html)及 [v10 raspi.c](https://raw.githubusercontent.com/qemu/qemu/v10.0.0/hw/arm/raspi.c)、[bcm2838.c](https://raw.githubusercontent.com/qemu/qemu/v10.0.0/hw/arm/bcm2838.c)：机器模型、spin-table、GIC/计时器接线与模型限制。

- [Raspberry Pi armstub8](https://raw.githubusercontent.com/raspberrypi/tools/master/armstubs/armstub8.S)：固件 SMPEN、CNTFRQ 与 GIC Group 1 初始化契约。
