# DRIVER 缺口

对应 V1-DRIVER。现状见 [current/drivers-implementation-status.md](../current/drivers-implementation-status.md)。

### DRIVER-09 STM32F103 平台文件含硬编码 MMIO 地址

- 现状：`shared_bus.c` 的 20 余处 `MMIO32(0x40…)` 已改为 `board_config.h` 里的命名寄存器宏（地址集中在该板的 memory map，平台 C 源不再含硬编码地址），`rg` 静态断言已通过，`shared_bus.c` 用本地解包的 `arm-none-eabi-gcc 14.2` 能为 `thumbv7m-none-eabi` 编译。`make check-stm32f103` 仍为红，但现在停在 `tools/rust-kernel.mk:3971` 的 ARMv7-M Rust 目标断言（DRIVER-10，delivery-owned），而不是本条的地址违规。
- 缺口：门禁要等 DRIVER-10 修复后才能跑完整构建；本机没有系统级 `arm-none-eabi-gcc`，CI 容器才有完整工具链。
- 关闭条件：DRIVER-10 修复后 `check-stm32f103` 通过。
- 来源：DELIVERY 的保留平台构建矩阵（[current/delivery-baseline.md](../current/delivery-baseline.md) §3）。

### DRIVER-10 ARMv7-M 档的内核 Rust 目标不完整

- 现状：`make stm32f103-bringup`（以及 `stm32f103-xuanwu`）在解析期失败，报 `tools/rust-kernel.mk:3971: the Rust module has no kernel Rust target for ARCH=armv7m`。本轮修掉了两处确定缺口（`RUST_WAIT4`、`RUST_WAIT_TIMER` 的 armv7m 条目——MCU profile 含 `kernel/proc/wait.c` 与 `kernel/proc/timer_heap.c`，两个对象实测能为 `thumbv7m-none-eabi` 编译），但门禁仍在 `RUST_VFS_LIFECYCLE` 处停下：它的目标继承自缺 armv7m 的 `RUST_EXT4_EXTENT_TXN`，而 VFS lifecycle 本身并不在 MCU 的源码表里。这类断言的写法是不带条件的 `$(error)`，所以对不在本次构建里的模块也会触发；`mm/mremap.c` 那一处是按 `KERNEL_SRC` 加了条件的，其余多数没有。
- 缺口：ARMv7-M 的内核固件无法构建，MCU 档没有构建证据；需要区分"MCU profile 真的要用这个 Rust 模块"（补 armv7m 目标）与"这个模块不在 MCU profile 里"（把断言按 `KERNEL_SRC` 加条件）。
- 关闭条件：`make stm32f103-bringup` 与 `make stm32f103-xuanwu` 编译通过，且 `check-stm32f103` 通过。
- 来源：同上。
