# DRIVER 缺口

对应 V1-DRIVER。现状见 [current/drivers-implementation-status.md](../current/drivers-implementation-status.md)。

### DRIVER-01 同源驱动的 kernel/user 双态

- 现状：完整 kernel 驱动与 user 驱动仍是两套实现。
- 缺口：没有一份生产驱动源码在两种部署模式下提供相同契约。
- 关闭条件：选一个完整驱动，probe/data/IRQ/DMA/removal 在两种模式下逐项一致，含越权 DMA 拒绝与清理。
- 来源：requirements N05。

### DRIVER-03 MSI / MSI-X 的剩余能力

- 现状：x86_64 的 MSI-X 能力解析、LAPIC 编程与 virtio/e1000e 接入已有；MSI（能力 0x05）全树未使用。
- 缺口：只带 MSI 的设备（AHCI/NVMe/xHCI/rtl8139）只能走 INTx 或轮询；非 x86 的 MSI-X 与 per-queue 亲和性未实现。
- 关闭条件：MSI 路径可用并按 MSI → MSI-X → INTx → 轮询降级；非 x86 有实现或明确的每设备类回退。
- 来源：requirements N10–N12、[current/server-readiness.md](../current/server-readiness.md) §八。

### DRIVER-07 ACPI `_PRT` 的剩余三项

- 现状：DSDT 求值器失败即关闭，绝不猜 GSI；QEMU 的 q35 走 swizzle。
- 缺口：桥后总线的 `_PRT` 下钻未实现（桥后设备留轮询）；i440fx PIRQ link 只读不写；解释器在真实固件上从未执行。
- 关闭条件：桥后设备能定位 GSI；PIRQ 可编程；至少一台真机完成 INTx 投递验证。
- 来源：[current/server-readiness.md](../current/server-readiness.md) §八 P1、[kernel/arch/x86_64-cpu-id.md](../../kernel/arch/x86_64-cpu-id.md)。

### DRIVER-08 板级存储/网卡驱动缺失

- 现状：LicheeRV Nano、Milk-V Duo、H616、RK3328 只到 build-verified；SD/eMMC 与 GMAC 均无可用驱动。
- 缺口：这些板子无法越过仅 RAM 启动；`dw_sdio.c` 覆盖的是更老的 DesignWare 布局。
- 关闭条件：至少一块板有可用的块驱动，能从卡启动并自然关机；或者明确移出 1.0 平台范围。
- 来源：[platforms/licheerv-nano.md](../../platforms/licheerv-nano.md)、
  [platforms/milk-v-duo.md](../../platforms/milk-v-duo.md)、
  [platforms/sun50i-h616.md](../../platforms/sun50i-h616.md)、
  [platforms/rk3328.md](../../platforms/rk3328.md)。

### DRIVER-09 STM32F103 平台文件含硬编码 MMIO 地址

- 现状：`make check-stm32f103` 报 `hardcoded STM32 MMIO address found`，逐条列出 `kernel/platform/stm32f103/shared_bus.c:9` 起的 20 余处 `MMIO32(0x40…)` 常量，门禁因此在候选上为红（在固定 CI 容器里复现，宿主无 `arm-none-eabi-gcc`）。
- 缺口：该板的平台文件绕过 MMIO 抽象直接写物理地址；这条门禁本就是为拦住它而存在的，所以门禁为红是真实违规而不是夹具问题。
- 关闭条件：这些访问改走平台的 MMIO 抽象后 `check-stm32f103` 通过。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）。

### DRIVER-10 ARMv7-M 档的内核 Rust 目标不完整

- 现状：`make stm32f103-bringup`（以及 `stm32f103-xuanwu`）在解析期失败，报 `tools/rust-kernel.mk:3971: the Rust module has no kernel Rust target for ARCH=armv7m`。本轮修掉了两处确定缺口（`RUST_WAIT4`、`RUST_WAIT_TIMER` 的 armv7m 条目——MCU profile 含 `kernel/proc/wait.c` 与 `kernel/proc/timer_heap.c`，两个对象实测能为 `thumbv7m-none-eabi` 编译），但门禁仍在 `RUST_VFS_LIFECYCLE` 处停下：它的目标继承自缺 armv7m 的 `RUST_EXT4_EXTENT_TXN`，而 VFS lifecycle 本身并不在 MCU 的源码表里。这类断言的写法是不带条件的 `$(error)`，所以对不在本次构建里的模块也会触发；`mm/mremap.c` 那一处是按 `KERNEL_SRC` 加了条件的，其余多数没有。
- 缺口：ARMv7-M 的内核固件无法构建，MCU 档没有构建证据；需要区分"MCU profile 真的要用这个 Rust 模块"（补 armv7m 目标）与"这个模块不在 MCU profile 里"（把断言按 `KERNEL_SRC` 加条件）。
- 关闭条件：`make stm32f103-bringup` 与 `make stm32f103-xuanwu` 编译通过，且 `check-stm32f103` 通过。
- 来源：同上。
