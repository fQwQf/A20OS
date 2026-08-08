# 驱动部署 Profile

A20OS 只有两种驱动部署 profile：`generic` 与 `embedded`。profile 描述制品如何部署，不改变统一的 `driver_t`、`device_t`、描述符或 probe/remove 语义。

## generic

除嵌入式板级镜像外，构建默认使用 `DRIVER_DEPLOYMENT=generic`。可选硬件必须以 `.a20drv` 包部署，由驱动管理器扫描并按描述符的 placement 激活：内核模块进入 drvmod，用户服务成为 native 用户进程。

generic 有两个 DriverStore 层次：

| 目录 | 目的 | 当前状态 |
|---|---|---|
| `/boot/drivers` | Early DriverStore；未来由 initramfs 提供，在挂载真实根盘前加载总线和根设备驱动 | 管理器已支持，可缺失 |
| `/bin/lib/drivers` | Runtime DriverStore；根文件系统可用后的常规模块与用户服务 | 当前 FAT32 镜像使用 |

Early DriverStore 是解除“模块在根盘上、根盘驱动又是模块”的 bootstrap 循环的唯一允许手段。启动策略可以指定早期加载顺序，但设备 identity、资源和 placement 仍只能出现在 ELF `.a20drv` 描述符中，不能恢复旁车 manifest。

启动生命周期已经分为两个实际入口：`driver_manager_early_init()` 在 `vfs_init()` 之后、`mount_block_devices()` 之前仅扫描 `/boot/drivers` 并只激活内核模块；`driver_manager_init()` 在 `proc_init()` 后注册 runtime board device 并扫描 `/bin/lib/drivers`，此时才允许生成用户服务。这样 early 包无法错误地在进程子系统尚未初始化时启动用户态代码。

generic 只把驱动核心、总线与内核服务链接进镜像，不保留任何内建设备驱动。`loop`、`udisk`、`pty`、`uart`、`framebuffer`/`gpu_core`、`audio_core`、`input_mux` 和 `usb_core` 是内核服务或聚合层，不是可迁移的设备驱动，不能被错误记入迁移完成。所有可发现设备驱动由 `mk/driver-modules.mk` 构建为 `.a20drv`：根盘相关驱动（virtio-blk、virtio-scsi、AHCI、DW SDIO）与 RTC 进入 Early DriverStore（`/boot/drivers`，root ramfs，真实根盘挂载前加载），其余进入 Runtime DriverStore（`/bin/lib/drivers`）。源码归属见 `mk/driver-sources.mk`：`GENERIC_KERNEL_SERVICE_SRCS` 列内核服务，`EMBEDDED_DEVICE_DRIVER_SRCS` 列 embedded 静态链接的完整驱动集。新增内建设备驱动必须显式进入源码账本并说明启动依赖。

板级设备通过 `platform_bus` 注册并携带 `hardware_id`，驱动按 platform id 匹配；驱动核心不再允许无总线设备被任意总线无关驱动抢占，无总线驱动必须提供显式 `match()` 才能绑定（UART 串口服务即按设备名匹配）。不得为了移动源文件而在模块中复制 transport、IRQ、DMA 或 class 服务实现。

## embedded

`DRIVER_DEPLOYMENT=embedded` 将完整驱动集静态链接到内核，不构建 `.a20drv` 包、不填充 DriverStore，也不运行驱动管理器扫描。STM32 是当前默认采用 embedded 的板级目标，但不是 profile 名或唯一适用对象；后续任何资源受限板级镜像都可使用它。

同一份驱动源和统一驱动核心同时服务两个 profile。embedded 不是第二个驱动模型，也不应产生只适用于 STM32 的部署 API。

## 构建检查

```sh
make ARCH=riscv64 print-driver-deployment
make ARCH=armv7m print-driver-deployment
make ARCH=x86_64 DRIVER_DEPLOYMENT=embedded kernel-only
```

输出同时给出该 profile 的模块数和 embedded 静态链接的设备驱动数。非 MCU 架构的 embedded 构建目录带有 `-embedded` 后缀，避免与默认 generic 对象复用；generic 与 STM32 保持既有目录名，兼容现有运行、烧录与评测入口。

构建规则按职责拆分：顶层 `Makefile` 保留全局变量、核心编译图和聚合入口；`mk/driver-deployment.mk` 定义 profile，`mk/driver-sources.mk` 维护驱动源账本，`mk/driver-modules.mk` 维护驱动包与 smoke，`mk/stm32.mk` 维护 STM32 专用入口，`mk/run-targets.mk` 维护运行、镜像和调试入口。
