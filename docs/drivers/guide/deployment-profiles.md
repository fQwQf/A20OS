# 驱动部署 Profile

A20OS 只有两种驱动部署 profile：`generic` 与 `embedded`。profile 描述制品如何部署，不改变统一的 `driver_t`、`device_t`、描述符或 probe/remove 语义。

## generic

除默认 embedded 的 `armv7m`/`ppc64le` 外，构建默认使用 `DRIVER_DEPLOYMENT=generic`。可选硬件必须以架构清单实际列出的 `.a20drv` 包部署，由驱动管理器扫描并按描述符 placement 激活：内核模块进入 drvmod；user-service 只能在 Runtime 阶段按白名单 MMIO 和 Native ABI 边界生成，带 `SUPERVISED` 的包只记录并交给外部 supervisor。

generic 有两个 DriverStore 层次：

| 目录 | 目的 | 当前状态 |
|---|---|---|
| `/boot/drivers` | Early DriverStore；构建时把选定 `.a20drv` 以只读 blob 链入 root ramfs，在挂载真实根盘前加载 | 当前由各架构 `early_drivers.c` 暴露，可缺失或为空 |
| `/bin/lib/drivers` | Runtime DriverStore；根文件系统可用后的常规模块与用户服务 | 当前 FAT32 镜像使用 |

Early DriverStore 是解除“模块在根盘上、根盘驱动又是模块”的 bootstrap 循环的唯一允许手段。启动策略可以指定早期加载顺序，但设备 identity、资源和 placement 仍只能出现在 ELF `.a20drv` 描述符中，不能恢复旁车 manifest。

启动生命周期已经分为两个实际入口：`driver_manager_early_init()` 在 `vfs_init()` 之后、`mount_block_devices()` 之前仅扫描 `/boot/drivers` 并只激活内核模块；`driver_manager_init()` 在 `proc_init()` 后注册 runtime board device 并扫描 `/bin/lib/drivers`，此时才允许生成用户服务。这样 early 包无法错误地在进程子系统尚未初始化时启动用户态代码。

generic 只把驱动核心、总线与 `GENERIC_KERNEL_SERVICE_SRCS` 的九个显式服务源码链接进镜像：`loop`、`udisk`、`pty`、`uart`、`framebuffer`、`gpu_core`、`audio_core`、`input_mux` 和 `usb_core`。设备包由 `tools/driver-modules.mk` 按架构列出，不能把 `EMBEDDED_DEVICE_DRIVER_SRCS` 自动等同于 generic 包集合。Early 集合也按架构不同；Runtime DriverStore 只接收该架构模块清单扣除 Early 集合后的包，以及构建规则列出的 user-service 包。当前 StarFive/LS2K GMAC 没有 generic 包，虽在 embedded 静态账本中，但 generic 板级构建不会获得它们。新增内核服务、embedded 驱动或 generic 包都必须分别进入对应显式账本并说明启动依赖。

板级设备通过 `platform_bus` 注册并携带 `hardware_id`，驱动按 platform id 匹配；驱动核心不再允许无总线设备被任意总线无关驱动抢占，无总线驱动必须提供显式 `match()` 才能绑定（UART 串口服务即按设备名匹配）。不得为了移动源文件而在模块中复制 transport、IRQ、DMA 或 class 服务实现。

## embedded

full profile 的 `DRIVER_DEPLOYMENT=embedded` 将 `EMBEDDED_DEVICE_DRIVER_SRCS` 与内核服务静态链接到内核，过滤掉 `driver_manager.c`，不构建 `.a20drv` 包，也不填充或扫描 DriverStore。该静态账本不是 generic 包的并集：RTC、PC speaker、PS/2、NVMe、TPM、HDA、vinput 和 vinput-probe 等 module-only 实现当前不会出现在 embedded 镜像。

MCU profile 是构建图例外：`armv7m` 强制 `PROFILE=mcu`，直接通配 `BOARD_DRIVER_DIR`（当前为 `kernel/drivers/stm32f1/*.c`）和板级平台源码，不消费 full profile 的 `DRIVER_KERNEL_SRCS`。`armv7m` 和尚无 drvmod 工具链的 `ppc64le` 当前都默认标记为 embedded，但二者的实际源码选择分别由 MCU 分支和 full-profile embedded 分支决定。其他架构可显式选择 embedded，但必须按实际构建分支核对所需能力。

模块包装通常通过包含 `kernel/drivers/` 下的共享实现并改写 `DRIVER_REGISTER` 来复用同一协议源码；只有在 `tools/driver-modules.mk` 明确提供包装和架构清单项时，这种双部署才成立。embedded 不是第二个驱动模型，也不应产生只适用于 STM32 的部署 API。

## 构建检查

```sh
make ARCH=riscv64 print-driver-deployment
make ARCH=armv7m print-driver-deployment
make ARCH=x86_64 DRIVER_DEPLOYMENT=embedded kernel-only
```

输出同时给出该 profile 的模块数和 `EMBEDDED_DEVICE_DRIVER_SRCS` 账本项数；对 MCU profile，后者不是实际 `BOARD_DRIVER_DIR` 源码数。非 MCU 架构的 embedded 构建目录带有 `-embedded` 后缀，避免与默认 generic 对象复用；generic 与 STM32 保持既有目录名，兼容现有运行、烧录与测试入口。

构建规则按职责拆分：顶层 `Makefile` 保留全局变量、核心编译图和聚合入口；`tools/driver-deployment.mk` 定义 profile，`tools/driver-sources.mk` 维护驱动源账本，`tools/driver-modules.mk` 维护驱动包与 smoke，`tools/stm32.mk` 维护 STM32 专用入口，`tools/run-targets.mk` 维护运行、镜像和调试入口。
