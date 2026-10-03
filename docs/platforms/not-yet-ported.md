# 尚未移植的目标：Kendryte K230 与 Allwinner D1s/F133

记录这两个目标**为什么没有**随 `licheerv-nano` / `milk-v-duo` / `rk3328` /
`sun50i-h616` / `x86_64-pc` 一起移植。两者都已经查到足够多的事实，但各自卡在一个
**不能靠猜解决**的地方。与其交付一个"能编译但上板必然失败"的板子，不如把阻塞点写
清楚。

本文所有事实同样转录自上游 Linux
commit `551c722f40809618230001baccf219193e22fc5a`。

---

## Kendryte K230 / K230D

### 已经确认的事实

| 项 | 值 | 来源 |
|---|---|---|
| CPU | 双核 T-Head C908（1.6 GHz 带 RVV 1.0 + 0.8 GHz），`RV64GCB` | `canaan/k230.dtsi`、Kendryte 产品页 |
| MMU | 有（C908 是 RVA23 核） | `compatible = "thead,c906", "riscv"` |
| PLIC | `interrupt-controller@f00000000`，0x04000000 | `canaan/k230.dtsi` |
| CLINT | `timer@f04000000`，0x10000 | 同上 |
| UART0 | `serial@91400000`，0x1000，IRQ 16，`"snps,dw-apb-uart"` | 同上 |
| DDR | K230D **片内 128 MB LPDDR4**；K230 最高 2 GB | `kendryte.com` K230D 产品页 |
| 启动 | mainline U-Boot 有 `k230_canmv` 板级目标，已启用 ns16550 与 DWC2 OTG | `docs.u-boot.org/en/v2026.04/board/canaan/k230_canmv.html` |

**PLIC 可以复用。** DTS 的 compatible 是 `"canaan,k230-plic", "thead,c900-plic"`，
回退项命中 `irq-sifive-plic.c` 的 `of_device_id`，所以
`kernel/drivers/irqchip/plic.c` 同样适用。UART 是 16550。这一半和 SophGo 两块板一样
干净。

### 阻塞点：PLIC 在 60 GiB，需要第三个 MMIO 巨页槽

K230 的 PLIC 在物理地址 `0xf00000000`（60 GiB）。而
`kernel/arch/riscv64/boot/entry.S` 建立的 Sv39 启动映射只有**两个** MMIO 槽：

```
entry.S:95   pgdir[256] → PA 0x00000000      (硬编码，1 GiB)
entry.S:96   pgdir[257] → PA BOOT_MAP_MMIO_HI (板级可设，1 GiB)
```

UART0 在 `0x91400000`（约 2.26 GiB），PLIC 在 `0xf00000000`（60 GiB）——**两者相距太远，
一个 1 GiB 槽装不下**。也就是说 K230 至少需要第三个巨页槽，这属于 `entry.S` 的架构层
改动，而不是写一个 `board.c` 就能解决的。

这类改动**必须上板验证**：写错启动页表的后果是内核在第一条指令前就取不到 MMIO，
而本仓库没有真机 CI 门禁。宁可不做，也不做一个"编译通过、上板必死"的板。

### 移植它需要先做的事

1. 给 `entry.S` 的启动映射增加可配置的第三个 MMIO 槽（或改为按需从设备树建立映射）。
2. DDR 训练依赖 Canaan 提供的 `firmware_gen.py`（U-Boot 文档明写 "Get the
   firmware_gen.py from vendor"），要先把它的输入格式搞清楚。
3. **PMP 上电即锁**：`thead,c906` 的 PMP 复位状态是锁定态，mainline OpenSBI 需要
   打补丁才能跳过（LKML 有公开讨论）。这会影响 S-mode 下的内存保护行为，必须查清
   A20OS 的 PMP 使用方式再动。
4. K230 的 mainline Linux 设备树是 2026 年中才并入 v7.3，驱动仍早期；没有稳定的
   上游文档可核对 RAM 窗口。

参考价 USD 40–50（CanMV-K230D-Zero 约 USD 25–42）。

---

## Allwinner D1s / F133

### 已经确认的事实

| 项 | 值 | 来源 |
|---|---|---|
| SoC | Allwinner D1s（F133），XuanTie C906，**`RV64IMAFDCVU`** | `allwinner/sun20i-d1s.dtsi` |
| CPU compatible | `"thead,c906", "riscv"` | 同上 |
| PLIC | `interrupt-controller@10000000`，0x4000000，`"allwinner,sun20i-d1-plic", "thead,c900-plic"`，`riscv,ndev = 175` | 同上 |
| UART0 | `serial@2500000`，0x400，`SOC_PERIPHERAL_IRQ(2)`，`"snps,dw-apb-uart"` | `sunxi-d1s-t113.dtsi` |
| UART1 / UART2 | `0x02500400` / `0x02500800`，IRQ+1 / IRQ+2 | 同上 |
| EMAC | `ethernet@4500000`，0x10000，`allwinner,sun20i-d1-emac` | 同上 |
| DDR | **64 MB DDR2 片内**（D1s/F133 的定义特征） | linux-sunxi.org D1s 页 |
| 启动 | OpenSBI + U-Boot；mainline Linux **6.3+** 已合入 D1/D1s 平台使能 | Phoronix 2023-01-01 |
| 板 | Sipeed Lichee RV（D1，512 MB）约 USD 17；F133-B SoC 约 USD 8.90–11.64（LCSC） | Sipeed、LCSC |

**PLIC 同样可以复用**：compatible 列表里的回退项 `thead,c900-plic` 命中标准 SiFive
驱动。CPU 的 `RV64IMAFDCVU` 是 A20OS 现有 `-march=rv64imafdc_zicsr_zifencei` 的**超集**，
架构层零改动。这一点和 SophGo 两块板完全一致。

### 阻塞点：两个关键字段无法从公开上游确定

1. **`SOC_PERIPHERAL_IRQ(nr)` 的偏移量未知。** `sunxi-d1s-t113.dtsi` 使用
   `SOC_PERIPHERAL_IRQ(2)` 这样的宏，但该宏的定义不在该文件里，也不在
   `include/dt-bindings/interrupt-controller/` 下按 `sun20i-d1` / `sunxi-d1` 命名的
   头文件中（已确认检索过）。SophGo 的 CV1800B 定义是 `nr + 16`，Allwinner 这边**不能
   类推**——猜错会让 UART0 的中断号整体偏移，表现为"固件能进内核但控制台永远不响"。

2. **RAM 基址未知。** `sun20i-d1-lichee-rv.dts` 等板级 DTS **都没有 memory 节点**
   （DDR 由 U-Boot 传入），而 `sun20i-d1.dtsi` 里也没有。Allwinner 平台通常把 DDR
   放在 `0x40000000`，但那是**惯例而不是可引用的来源**。这一项和
   `sun50i-h616` 的 `PHYS_MEMORY_END` 是同一类问题，只是更严重：H618 至少有一个可
   依据的惯例值，D1s 这里连惯例都要靠猜。

   RAM 基址猜错的后果比 IRQ 号更糟：分配器会去碰并不存在的物理内存。

### 移植它需要先做的事

1. 从 Allwinner BSP 的 `dt-bindings` 或 U-Boot 的 D1 defconfig 里确定
   `SOC_PERIPHERAL_IRQ` 的偏移，以及 DDR 基址；两者都要有可引用的出处。
2. 存储：Allwinner 自家的 MMC 控制器（A20OS 只有 `dw_sdio.c`，对应 DesignWare
   SDIO/MSHC），和 H618 一样需要新驱动。
3. EMAC 是 `allwinner,sun20i-d1-emac`，厂商 IP，需要新驱动。

**注意**：D1s/F133 的 64 MB 与 Milk-V Duo 同量级，即使移植成功也只能跑
`BRINGUP=1 RAMFS_USER=1`。如果目标是"最便宜的能跑 A20OS 的板子"，
`licheerv-nano`（USD 9–14 / 256 MB）严格优于它——D1 主板（Sipeed Lichee RV，
512 MB，USD 17）反而更值得考虑，但那是另一颗 SoC、另一套 boot 介质。

---

## 为什么这些字段不能猜

仓库有一条硬性约定（`docs/security/hardening.md`，由 `make check-honesty-policy`
强制）：**未实现或未核实的能力必须如实报告"不存在"，不得编造输出**。
把未核实的地址写进 `PHYS_MEMORY_BASE` 属于同一类问题——它会编造出一个"看起来能工作"
的启动路径，而失败点被推迟到没有串口日志可查的时刻。

因此本文选择记录阻塞点，而不是交付占位实现。已经移植的板之所以敢写
`KERNEL_ENTRY`，是因为它们要么沿用了树里已验证的值（如 RAM 在 `0x80000000` 的
SophGo 两块板直接用 riscv64 默认值），要么把该字段明确标成 `UNVERIFIED` 并在文档里
指出它是上板第一件要核对的事（见 `licheerv-nano.md`、`milk-v-duo.md`、
`sun50i-h616.md`）。