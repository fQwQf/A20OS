# Virtio-console 内存配置

`virtio-console.a20drv` 提供 raw `/dev/vport0` 和带背压的批量 TTY。队列、DMA 缓冲区及来客接收字节环均可在构建时配置。默认 `throughput` 保持当前 CLI 吞吐配置；内存紧张的目标可选择 `compact`。

## 架构边界

该设备通过 CHAR primary protocol 和既有专用 virtqueue 提供字节流；下述 TX 槽背压/完成所有权没有被通用 request queue 自动替换。调用者的字节只借用到方法约定的范围，设备仍持有的 DMA 槽须由后端继续保留。绑定、active call 和存储 owner 的通用规则见[核心模型](../guide/core-model.md)，新增执行机制见[请求队列](../guide/request-queues.md)。配置只改变容量，不授予提前复用或停止失败后释放 DMA 的权限。

## 实例配置

在目标 `instances/<名字>.toml` 的现有 `[kernel]` 段加入：

```toml
[kernel]
virtio_console_profile = "compact"
```

然后正常校验、构建并运行该实例，例如修改 `cli-x86_64.toml` 后：

```sh
tools/a20 check cli-x86_64
tools/a20 build cli-x86_64
tools/a20 run cli-x86_64
```

这会重新打包驱动及相关镜像，无需 `make clean`。省略该字段或填写`"throughput"` 恢复默认。配置固化在驱动模块中，不能通过 bootargs 运行期修改。`[machine].console = "virtio"` 负责选择 QEMU 批量 TTY 路由，与内存配置独立。

| 参数 | `throughput`（默认） | `compact` | 允许值 |
|---|---:|---:|---|
| `virtio_console_rx_slots` | 8 | 2 | 1、2、4、8、16 |
| `virtio_console_tx_slots` | 16 | 2 | 1、2、4、8、16 |
| `virtio_console_buffer_size` | 4096 B | 512 B | 64～4096 的 2 次幂 |
| `virtio_console_ring_size` | 4096 B | 512 B | 128～65536 的 2 次幂 |

可在预设之上单独覆盖参数；未填写的值继承所选预设。例如：

```toml
[kernel]
virtio_console_profile = "compact"
virtio_console_rx_slots = 2
virtio_console_tx_slots = 4
virtio_console_buffer_size = 512
virtio_console_ring_size = 1024
```

`buffer_size` 是每个 RX/TX 槽的数据容量，`ring_size` 是驱动内部的接收字节环容量。减少 TX 槽或缓冲区可能增加等待与通知次数；缩小接收环降低突发输入容纳量。满队列、超时、信号及 drain 的 DMA 所有权规则保持一致：设备完成前不会复用槽。`tools/a20 check` 和 Make 构建均会拒绝越界值及非 2 次幂值。

## 直接使用 Make

实例字段对应以下变量：

```sh
make ARCH=x86_64 BOARD=qemu-virt-x86_64 NR_CPUS=2 \
    VIRTIO_CONSOLE_PROFILE=compact dev-build
```

四个覆盖变量分别为 `VIRTIO_CONSOLE_RX_SLOTS`、`VIRTIO_CONSOLE_TX_SLOTS`、`VIRTIO_CONSOLE_BUFFER_SIZE`、`VIRTIO_CONSOLE_RING_SIZE`。直接使用 Make 时，各次构建应传入同一组配置；下一次省略变量将恢复默认值。驱动构建生成 `USER_BUILD_DIR/.virtio-console-config.h`，只在解析后的值变化时更新文件，从而触发模块及镜像重建。不要手工编辑此文件。

配置针对可加载模块。自行把驱动加入静态源码账本时，可通过`CONFIG_VIRTIO_CONSOLE_RX_SLOTS`、`CONFIG_VIRTIO_CONSOLE_TX_SLOTS`、`CONFIG_VIRTIO_CONSOLE_BUFFER_SIZE`、`CONFIG_VIRTIO_CONSOLE_RING_SIZE`编译宏设置同样的值；默认和编译期校验位于`kernel/include/drivers/char/virtio_console_config.h`。当前 embedded 源码账本未包含该驱动。

## 内存预算

以下为当前 x86_64、4 KiB 页、单实例的 **DMA 实际分配量**：

| 模式 | `throughput` | `compact` |
|---|---:|---:|
| raw `/dev/vport0`（RX + TX） | 192 KiB | 8 KiB |
| 批量 TTY（另含两个控制队列） | 200 KiB | 16 KiB |

每个数据队列把描述符、avail/used ring 和数据放在同一个 coherent 块中；当前分配器按 2 次幂页数向上取整，因此不能仅以“槽数 × 数据容量”估计占用。例如默认 RX 结构略超 32 KiB，实际分配 64 KiB；默认 TX 略超 64 KiB，实际分配 128 KiB。compact 的两个数据队列各占 4 KiB，控制队列也各占 4 KiB。表格不含模块代码、BSS、设备框架元数据；接收字节环和 echo 缓冲区位于 BSS，也随配置缩小。其他页大小或架构需要重新计算。

没有匹配设备时不分配这些 DMA 块。但 generic DriverStore 会加载其中的模块，模块代码和 BSS 仍有开销。完全不需要 virtio-console 的目标，应在`[rootfs].drivers` 中显式列出所需驱动并排除 `virtio-console`（见[实例的驱动选择](../../dev/instances.md#组件注册表componentsdriverstoml)）。仅把 `machine.console` 改为 `serial` 不会移除模块，也不会阻止已有 VirtIO 设备的 probe。管道默认容量及宿主中继缓冲区不受这些配置影响；宿主中继占用的是宿主内存。

## 验证

`test_virtio_console_config.py` 校验预设、覆盖、实例到 Make 的映射、增量生成及生产队列结构的 DMA 预算。`test_vport_tx.py` 在 throughput、compact 和最小合法配置下验证并发、索引回绕、超时、信号和设备持有 DMA 时的所有权。

compact 已在 x86_64 KVM 下通过 2 MiB 逐字节输出核验、接收端暂停 250 ms、 Ctrl-C，以及管道、poll/epoll、160 条并发 TTY 记录和 `tcdrain` 回归。本地记录为 `build/vport-config/compact-runtime/results.json`，不纳入源码。最小合法配置（RX/TX 各 1 槽、64 B 缓冲区、128 B 接收环）也通过 512 KiB 逐字节输出和同组回归，记录为 `build/vport-config/minimum-runtime/results.json`。性能复测方式参见自动化基准脚本 `tools/mk/targets-bench.mk`；小内存预设不承诺与默认预设相同的吞吐量。


## 停止与失败保留

驱动在初始化前登记受管失败 probe 和 execution stop。停止先关闭准入，使发送等待返回；正在运行的 class 调用与 IRQ RX pump 持活动引用，覆盖锁外通知和 UART 递送。尚有调用或锁忙时返回 `-EBUSY`。只有公共 reset 确认 status=0 后才同步退出 IRQ， class 排空后 remove 释放 RX/TX/控制队列 DMA 与单实例 owner；超时、失联或失败握手都保留原绑定供重试，第二个 probe 不能覆盖失败现场。用户 ioctl 和数据契约不变。

本轮生命周期模型、原 TX 模型与实际 raw PCI 收发范围见驱动状态。实际收发/重开/drain 成功；同轮关机的 `xfsettingsd` SIGSEGV 单独保留为 DESKTOP-15，该轮不计作桌面整体验收通过。
