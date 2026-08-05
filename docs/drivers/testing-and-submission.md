# 构建、测试与提交驱动

> 不要这样做：不要只运行 `make` 就提交，不要只附截图，不要跳过构建矩阵，也不要忘记更新 `lock-order.md`。

驱动的完成标准不是“文件能编译”，而是枚举、匹配、失败回滚、类消费和目标硬件 I/O 都有证据。下面是外部贡献者可执行的完整流程。

## 把驱动接到构建里

full profile 自动包含 `kernel/drivers/core|bus|block|char|net|gpu|audio|input/*.c`。把源文件放到已有类目录即可。公共声明放 `kernel/include/drivers/<class>/`；只在一个驱动内部共享的寄存器和私有结构放驱动私有头，不要进入公共 include 树。

MCU profile 只编译 `BOARD_DRIVER_DIR` 和选中的平台，新增文件要检查 Makefile 的 profile 分支。新平台还需要平台链接脚本和 `BOARD_INCLUDE_DIR`。

`#ifdef CONFIG_<ARCH>` 只应包围真正架构限定的 I/O（例如 x86 port I/O）。可跨架构的 PCI/MMIO 驱动不要按板名条件编译。

## 最小静态验证

```sh
git diff --checkmake check-driver-core-modelmake check-doc-driftmake smoke-driver-lifecyclemake smoke-hdamake smoke-audio-userspacemake PYTHON='conda run -n a20os python' smoke-virtio-soundmake smoke-pci-portability
```

`smoke-driver-lifecycle` 用 RISC-V64 bringup 配置和 `CONFIG_DRIVER_LIFECYCLE_TEST=y` 启动合成 bus/device，验证注册、probe 失败解绑、class 发布、unregister 下线、陈旧引用返回 `-ENODEV` 和重新 probe；宿主需要能运行仓库配置的 QEMU。只改平台私有轻量设备时可以不跑它，但修改 driver core、bus 或生命周期代码时必须跑。

`smoke-hda` 在 x86_64 q35 上挂载 Intel HDA controller 和 duplex codec，验证 codec 拓扑识别、audio class 绑定以及一段静音 PCM 的 BDL DMA 完成。该测试使用 QEMU null audio backend，不依赖宿主声卡。

`smoke-audio-userspace` 构建完整 x86_64 用户态，在来宾 shell 中执行 `audioplay --tone 440 --duration 5000`，再由 QEMU WAV backend 捕获 HDA 输出。测试同时检查驱动绑定、命令成功、正常关机和 WAV 中的非零 PCM 采样，因此覆盖 UAPI、devfs、用户态短写循环、持续 HDA DMA 与 QEMU codec 的完整链路，且不依赖宿主声卡服务。

`smoke-virtio-sound` 使用同一个五秒用户态负载，但只挂载 QEMU `virtio-sound-pci`。它验证 modern PCI transport、PCM_INFO、SET_PARAMS/PREPARE/START、TX completion、DRAIN 后 STOP/RELEASE，以及 WAV 非静音和采样连续性。

`smoke-pci-portability` 在 LoongArch64 QEMU virt 上同时挂载 HDA 和 NVMe，验证未分配零值 BAR 的 sizing/assignment、HDA PCM DMA、NVMe queue/Identify 以及两个 class 绑定。目标创建专用 128 MiB 可丢弃镜像，写入超过单次 8 KiB bounce chunk 的数据，再执行 flush、读回和比较。`CONFIG_NVME_SMOKE_TEST` 会从 LBA 0 开始改写介质，只能由该目标配合自动生成的镜像启用。

检查源中没有板级硬编码泄漏：

```sh
rg -n "0x[0-9a-fA-F]{8}" kernel/drivers/<class>/my_driver.crg -n "CONFIG_BOARD_" kernel/drivers/<class>/my_driver.c
```

常量可能是协议寄存器或 ID，需要逐项解释，不是机械删除。

## 架构构建矩阵

至少构建驱动目标平台和一个共享基础设施回归平台：

```sh
make ARCH=aarch64 BOARD=virtualbox-aarch64 ABI=both kernel-only -j4make ARCH=aarch64 BOARD=qemu-virt-aarch64 ABI=both kernel-only -j4make ARCH=x86_64 ABI=both kernel-only -j4make ARCH=arm32 BOARD=qemu-virt-arm32 ABI=both kernel-only -j4make check-stm32f103
```

跨架构结论分三级记录：只编译通过；设备枚举并绑定；实际 DMA/I/O 完成。只有第三级可以写“运行验证”。某架构的 board 没有 PCI enumerator 时，HDA/NVMe 编译成功只能计入第一级。

不相关架构可按改动范围缩减，但修改 driver core、class、PCI、VirtIO 或 hwapi 时应扩大矩阵。本地门禁入口见 [testing/testing-gates.md](../testing/testing-gates.md)。

## probe 测试表

| 场景 | 预期 |
|---|---|
| ID 不匹配 | probe 不调用，设备保持 UNINIT |
| 必需 BAR/MMIO/IRQ 缺失 | 返回 `-ENODEV`，无 ready/私有指针 |
| DMA 第 N 次分配失败 | 前 N-1 次全部释放 |
| feature/version 不支持 | 设备 FAILED/复位，无 queue 泄漏 |
| request_irq 失败 | 显式降级为轮询且设备级中断保持屏蔽；不允许实例保持“看似 IRQ 驱动”的状态 |
| IRQ 已注册 | 启动日志或调试计数证实中断真实触发；共享线场景两个设备都完成 I/O；remove 后 probe 能重新注册 |
| 健康检查超时 | `-ETIMEDOUT`，设备停止后才释放 DMA |
| 成功 | `drv_priv`、class、容量/MAC/模式均有效 |

核心合成测试由 `CONFIG_DRIVER_LIFECYCLE_TEST` 覆盖基本 register/probe failure/unregister/reprobe。复杂驱动应增加可注入失败点或独立 host-side 静态门禁。

## I/O 测试

QEMU GUI 路径必须跑：

```sh
make smoke-qemu-gui-x86_64 SMOKE_TIMEOUT=60make smoke-qemu-gui-riscv64 SMOKE_TIMEOUT=90make smoke-qemu-gui-aarch64 SMOKE_TIMEOUT=90make smoke-qemu-gui-arm32 SMOKE_TIMEOUT=90make smoke-qemu-gui-loongarch64 SMOKE_TIMEOUT=90
```

这些门禁不依赖宿主图形会话。它们覆盖 PCI 和 VirtIO-MMIO transport，以及 32/64 位用户态，以 headless display 启动 GUI rootfs，要求 VirtIO GPU、两个 VirtIO input 实例和用户态 desktop 全部就绪；通过 QMP 抓取实际 scanout 并拒绝纯黑/空 framebuffer；最后注入按键并要求客户机产生 input event。日志和 PPM 截图保存在 `.kernel-build/smoke/qemu-gui-x86_64/`。因此“能链接”或“串口能启动”不能替代此项验证。完整门禁说明见 [testing/testing-gates.md](../testing/testing-gates.md)。

block：首尾 LBA、越界、零长度、跨内部 chunk、读后写回、flush、错误恢复。不要在装有唯一数据的镜像上做破坏性测试。

network：最小/最大帧、ring full、RX 错误、连续 wrap、无包 poll、链路断开；验证 send 返回字节数，recv 无包返回 0。

input：按下/释放、modifier、多事件 buffer、ring wrap、SYN_REPORT、无事件 `-EAGAIN`、remove 后 read。

display：模式信息、pitch、全屏和边界矩形 flush、映射重叠拒绝、可见 backing 长度、remove 后 ioctl。

## 并发与清理

审查每一个共享字段由哪把锁保护，并更新 `lock-order.md`。确认任何 spinlock 区间内没有 `kmalloc`、VFS、`sched`、`mdelay` 或长轮询。触发 remove 时先禁止新 I/O，再处理 IRQ/waiter/DMA。运行 probe-remove-probe，确保静态槽和全局默认项可以复用。

## 日志证据

提交说明应附从枚举到消费的连续日志，例如：

```text
[BUS] pci ... id=8086:100e ...[E1000] ready: mac=... link=up[LWIP] netif en0 attached to ...
```

日志要包含构建命令、平台/VM 设备配置、成功 I/O，以及已知未验证能力。不要只附截图；串口文本更适合评审和回归比较。

## 门禁失败时怎么办

| 失败门禁 | 先检查什么 | 下一步 |
|---|---|---|
| `git diff --check` 报错 | 行尾空格、tab 混用、文件尾空行 | 修复并重新 `git diff --check` |
| `make check-driver-core-model` | driver core 头文件、ID 表、class ops 是否匹配规范 | 对照 [drivers/core-model.md](core-model.md) 和 `kernel/include/drivers/` 修正 |
| `make check-doc-drift` | 文档与代码中同名常量、命令或矩阵不一致 | 同步文档和实现，确保命令矩阵和真实 Makefile 目标一致 |
| `make smoke-driver-lifecycle` 失败 | 合成 bus/device 注册、probe 失败清理、unregister 路径 | 加 `CONFIG_DRIVER_LIFECYCLE_TEST=y` 日志，确认失败点是否释放资源 |
| 构建矩阵中某一架构失败 | 是否用了 `#ifdef CONFIG_BOARD_` 或架构私有头 | 把板级常量移到 platform，把可跨架构代码改成通用 PCI/MMIO |
| `make smoke-qemu-gui-*` 失败 | QMP 截图是否全黑、input 事件是否注入成功 | 先跑对应 `kernel-only` 构建，确认 PCI/VirtIO 枚举和 class 绑定 |
| 块/网络/input/display I/O 失败 | 是否用了唯一数据镜像、是否满足类接口语义 | 用可丢弃镜像复跑，按设备类规范逐个检查返回值 |
| 硬件验收失败 | 串口日志是否包含从 `[BUS] pci` 到类消费者的完整链路 | 不要只以“桌面黑了”或“shell 出来了”作结论 |

如果同一门禁在改动前后行为不同，先回退到已知通过的 commit，确认门禁本身没有环境变化。不要通过放大超时或放宽断言来“绕过”失败；失败点通常指向真实的资源泄漏或竞争。

## 提交清单

- [ ] ID、资源大小、DMA 地址宽度来自硬件手册并已验证。
- [ ] 驱动不依赖板级地址或架构私有 include。
- [ ] class 返回值符合 [device-classes.md](device-classes.md)。
- [ ] probe 每个失败点有逆序清理，remove 可重复。
- [ ] IRQ、DMA、barrier、cache ownership 正确。
- [ ] 多实例状态不错误地放在单全局对象；若暂限单实例已记录。
- [ ] 新锁已加入 `lock-order.md`。
- [ ] 用户 ABI 由通用适配层实现并有边界检查。
- [ ] 文档、[实现状态](implementation-status.md)、构建和目标硬件日志一同更新。

提交前按本页的命令矩阵跑完相关门禁，并把日志贴到 PR 描述或提交说明里。
