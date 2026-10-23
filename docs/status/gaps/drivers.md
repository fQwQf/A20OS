# DRIVER 缺口

对应 V1-DRIVER。现状见 [current/drivers-implementation-status.md](../current/drivers-implementation-status.md)。

### DRIVER-08 控制台 UART 的接收不是真正中断驱动

- 现状：8250 在 `arch_uart_init()`（`kernel/arch/riscv64/include/console.h`）里写了 `IER = 0x01`（RX 中断使能），`uart_init()` 对 `UART0_IRQ`（QEMU virt 为 10）注册了 `uart_irq_wrapper`，`kernel/arch/riscv64/trap/irqchip.c` 也在 PLIC 里按 hart enable 了该源。但实测行为是「只有读者轮询才取字节」：前台跑 `ping`（不读终端）时按 ^C，收端行规一次 ISIG 都没有触发；等 `ping` 结束、shell 重新读终端时那个 `0x03` 才被取走。也就是说接收实际靠 `uart_getc()` 里的 `arch_uart_poll_getc()` 推进，RX 中断没有在起作用（是 `uart_handle_irq` 未被调用，还是 trap 分发没有把外部中断转进来，尚未定位）。
- 缺口：控制台输入在「没人读」的时候不被处理，于是 echo/ISIG 这类收端行规事件只能发生在有读者的瞬间——提示符上按 ^C 有效，前台作业不读终端时按 ^C 无效。这与 [drivers/guide/device-classes.md](../../drivers/guide/device-classes.md) 写明的契约（"UART 硬件驱动只负责收发和中断"）不符，属于驱动未完成项，而不是 tty 层的缺陷。
- 关闭条件：在没有任务读控制台的情况下，RX 字节仍进入环形缓冲区且 `rx_irq_bytes` 在没有 poll 的情况下增长；由此前台不读终端的作业也能被 ^C 打断（SIGINT 投递到该终端的前台进程组）；有一条门禁覆盖（例如前台跑 `ping`/`sleep` 时注入 ^C，断言作业在 2 秒内结束），并在 QEMU 与至少一块真机上通过。
- 来源：2026-10 实测（阻塞点记录见 [abi-ipc.md](abi-ipc.md) ABI-08）、[drivers/guide/device-classes.md](../../drivers/guide/device-classes.md)、`kernel/arch/riscv64/include/console.h`、`kernel/arch/riscv64/trap/irqchip.c`、`kernel/drivers/char/uart.c`。
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

### DRIVER-SECURITY-03 用户 MMIO 逐页映射失败缺少回滚

- 现状：`kernel/drivers/core/udriver.c::udriver_map_mmio()` 逐页调用 `pt_map()`，全部成功后才填写 VMA 的 start/end 并发布。若后续页分配/映射失败，fail 分支只有 `vma_put()`，没有撤销此前成功安装的 PTE，也没有完成 deferred VMA 清理。
- 缺口：后端返回失败后，用户地址空间可能保留没有 VMA 的部分设备映射；这违反 SECURITY-03 的拒绝无副作用与资源释放契约。Native ABI 现已修复成功映射后的输出拷贝失败回滚，但该修复不覆盖后端内部的部分失败。
- 关闭条件：由 DRIVER/MM owner 审查映射事务、锁序及 TLB 生命周期，撤销已安装部分并释放临时 VMA；在至少已映射一页后注入 `pt_map` 失败，确认没有剩余设备 PTE、临时对象/页表资源泄漏，正常映射与释放仍通过。
- 来源：SECURITY 包为 SECURITY-03 审查 Native MMIO 输出失败清理时确认的源码路径（2026-10-23）。逐页分配失败尚未做 guest 故障注入，不计作 PASS。本包未修改驱动/MM 源码。
