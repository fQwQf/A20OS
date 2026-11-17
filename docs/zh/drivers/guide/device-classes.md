# 设备类接口

设备类提供块、网络、字符、输入、显示和音频的稳定消费语义。完整结构和成员顺序以 `kernel/drivers/core/driver_class.h` 为准；音频表另见 `kernel/include/drivers/audio/audio_core.h`。本页规定单位、成功值、存储借用与类特有约束，不再维护各类重复的 probe/remove 模板。完整生命周期使用[核心模型](core-model.md#可编译的-pci-网络驱动骨架)或[原生请求与队列](request-queues.md#接入骨架)。

## 原生协议与传统 class

驱动选择 `driver_t.class_type` 并提供匹配的 class_ops；核心为六类自动发布相应 primary protocol。无传统 class 的服务可在 probe 中用 `device_add_protocol()` 发布独立协议。endpoint 与现有 publication 共用身份、引用和 active call，没有第二个设备注册表。

consumer 按协议 ID、语义版本、实际 ops 前缀和功能位取得 access，借用 caller 的 endpoint 引用直到 end；离线后新调用失败，旧引用不转向重绑。独立扩展优先使用新协议，而不是扩大所有设备的 vtable。协议 capability 不是用户 rights。具体入口见[原生协议](core-model.md#原生协议组合能力而非扩大全局-vtable)。

## 通用调用规则

| 问题 | 约定 |
|---|---|
| 参数与返回值 | 验证实例、buffer、长度/偏移与溢出；成功值按各类约定，失败为负 errno |
| 未实现操作 | 可选 callback 留 NULL；已知但不支持的能力返回 EOPNOTSUPP，未知 ioctl 返回 ENOTTY，不报告空成功 |
| 调用寿命 | device 引用只保留元数据；通过 class adapter 或成功的 protocol/class access 调用 ops，不缓存裸模块表 |
| 异步数据 | class buffer 通常借用到本次调用返回；要继续 DMA，必须复制或取得独立 backing/mapping owner |
| 用户指针 | read/write 为内核 buffer；未知 char/block ioctl 的原始 arg 尚未统一封送，需显式 usercopy，不能直接解引用 |
| 退出 | execution stop 在调用排空前安全解决异步执行；remove 成功后才回收 devres，失败保留绑定与资源 |

`device_find_get_by_class(class, rank)` 返回带引用的设备，rank 是在线列表位置；最终 device_put。class_device_get_by_type/class_device_get_nth 同样是在线 rank，class_device_get_by_index 查询当次 publication 的稳定索引。以上查找不代替在线 access。char/block/audio 自动发布动态 devfs 节点，display/input 使用既有聚合入口；节点和用户 ABI 见[用户接口与 devfs](../classes/userspace-and-devfs.md)。

## Block：`DEV_CLASS_BLOCK`

操作表为 `block_dev_ops_t`，包含 read、write、capacity、sector_size、flush 和 ioctl。

- lba/sectors 均以逻辑扇区计；capacity 返回总扇区数，sector_size 返回每扇区字节数。
- read/write 整次成功返回 0，失败为负 errno；不能以 0 表示部分完成。核对容量与乘法溢出，超出硬件命令长度时由后端拆分。
- flush 返回 0 表示此前成功写入已达到约定的持久性；只有硬件无易失缓存或保证写穿时才可 no-op。
- `/dev/diskN` 和文件系统 block adapter 由通用层提供，不新增驱动私有 block_dev_t getter。
- 通用 native queue 可用于新异步后端；既有 VirtIO/NVMe/AHCI 专用队列是否迁移由实际实现决定，不能由框架新增接口推断已迁移。

参考 `virtio_blk.c`、`virtio_scsi.c`、`nvme.c`、`ahci.c`。初始化与退出遵循共同生命周期，停止/映射失效未确认时保留 DMA，不在失败标签中直接 free。

## Network：`DEV_CLASS_NET`

操作表为 `net_dev_ops_t`。lwIP 消费者至少需要 send、recv、mac；可选 open/stop、poll、ioctl、link_up、rx_irq_driven、send_sg、caps 以当前头文件为准。其他网络后端通过公共适配层接入。

- send 成功返回帧字节数，队列满返回 EAGAIN；recv 返回一帧长度，当前 lwIP 适配无包返回 0。
- recv 不把一帧拆成多次返回；容量不足时的丢弃/EMSGSIZE 策略由 provider 明确。
- mac 返回六字节借用地址，仅在受保护调用范围内读取/复制，不在 consumer 长期保存私有指针。
- send_sg 的各段只借用到方法返回；返回成功不允许后端继续访问 caller 的栈/pbuf，需要独立的 TX 存储 owner。
- caps 只报告已协商且实现的能力。checksum/逐帧元数据需要 stack 与 provider 共同实现，不能仅设置位。
- lwIP 锁下调用的 send/poll 必须非阻塞、不分配、不重入 lwIP；设备 IRQ/RX 释放私有锁后再进入 stack，详见[锁契约](lock-order.md)。

NET_STATS 是独立协议扩展，VirtIO-net、RTL8139 已提供；consumer 用 `class_net_read_stats()` 获取按绑定保护的快照。错误输出虽会清零，返回错误仍表示不可用，不能作为成功的零计数。网络 stop 是数据面方法，不替代核心 execution stop、call drain 与最终 remove。

## Character：`DEV_CLASS_CHAR`

操作表为 `char_dev_ops_t`，read/write 返回实际字节数，非阻塞无进展返回 EAGAIN；poll 观察 readiness，不消费数据或睡眠。未知 ioctl 返回 ENOTTY；原始 arg 需按已知 ABI 明确封送。

类默认生成 `/dev/charN`；probe 内可用 `device_set_devfs_name()` 选择本实例名称（例如 vport0）。名字复制到 publication，拒绝空、超长、含斜杠或冲突的名字，发布后不能改名。它不提供任意 vnode/权限/用户 ABI 注册。名称冲突属于 publication 失败，进入可失败退出；不能保证硬件清理一定一次成功。

UART 的寄存器、收发和 IRQ 属于硬件实现，tty 层负责 termios、行编辑与前台进程组。参考[用户接口](../classes/userspace-and-devfs.md)与[virtio-console](../classes/virtio-console.md)。

## Input：`DEV_CLASS_INPUT`

操作表为 `input_dev_ops_t`，read/ioctl/poll 提供事件消费。事件 ABI 使用 `kernel/include/drivers/input/virtio_input.h` 中的 `struct input_event`；时间字段随 CONFIG_64BIT 选择 64/32 位，不能复制一个固定 32 位结构当作所有架构的线格式。

- read 只返回完整事件的字节数，容量不足返回 EINVAL，无事件返回 EAGAIN。
- poll 只观察 readiness；一批变化以 EV_SYN/SYN_REPORT 结束。
- 每实例维护事件 ring，溢出不静默覆盖旧事件；记录丢弃并按子系统支持的方式报告。
- `/dev/event0` mux 通过受保护的 class 调用聚合输入。IRQ 入队后可调用 `input_mux_wake()`，不需要私有硬件 getter。
- 退出先关闭硬件/URB producer 并确认回调排空，才释放 ring/实例；HID endpoint 交还遵循 USB 的可失败协议。

参考 `vinput.a20drv` 与 USB HID。PS/2 的既有控制台桥有单独边界，新 input 实现优先走标准 class，见[输入子系统](../classes/input.md)。

## Display：`DEV_CLASS_DISPLAY`

`gpu_dev_ops_t` 的基础模式/刷新、VirtIO/virgl 扩展、owning scanout 和原生角色均在完整头文件中定义；其中设备专有 3D 回调不是所有原生 GPU 的提交协议。

- probe 初始化部分实例和停止路径，准备缓存角色后调用 `gpu_device_register()`；核心随后发布 class。
- 默认输出要求 SCANOUT，优先 RENDER，同级优先非 BOOT_SCANOUT；headless 可发布但不占默认输出。
- get_info 返回当前模式。get_fb 返回借用的 CPU 物理视图，既不拥有 backing，也不授予 DMA 权限。
- RAM framebuffer 优先通过 get_scanout 交出 owning VMO；消费者可结束 access 后由 snapshot/VMA 继续保留 RAM。
- execution stop 在 call drain 前确认停止并安全退休请求，remove 再注销默认选择和交还 owner。raw 物理 VMA 没有自动撤销协议，不能以 offline 代替 backing 安全退休。

详细接口、能力和映射规则见[Display/Framebuffer](../classes/display.md)。DRM 的 per-publication/per-file 归属与 native queue 的请求归属互补；厂商 BO/GPUVM、抢占和恢复仍由子系统/后端实现。

## Audio：`DEV_CLASS_AUDIO`

操作表为 `audio_dev_ops_t`，以 caps/pcm_format 描述能力，read/write、set_format、tone、stop、drain、ioctl、poll、close 按支持范围实现。完整声明及用户结构分别见 audio_core.h 与 uapi/a20/audio.h。

- `/dev/audioN` 的编号不表示 PCM/tone 能力，consumer 先 GET_CAPS。
- PCM write 返回接受的字节数，允许短写；DRAIN 等待已接受数据，STOP 终止当前流/generation。
- audio_core 负责通用 ioctl usercopy 和 close 的 drain/stop 策略，驱动实现硬件动作。
- stream STOP 与绑定 execution stop 不同；后者还须覆盖 IRQ、后台工作、DMA 与失败初始化。现有音频后端有各自 ring/等待路径，不宣称已迁移 native queue。

格式、HDA/VirtIO PCM 进度及当前功能限制见[音频子系统](../classes/audio.md)。

## 新增设备类

先判断能否通过既有 class 加独立协议表达需求。确需新增类时，明确跨硬件/独立 consumer 的稳定语义，规定单位、上下文、并发、错误和异步存储所有权；实现 consumer、publication/primary protocol、适用配置与必要的用户 adapter，并验证完整接入。改变既有共享布局按模块 ABI 规则处理，不用厂商命令 blob 或一个可选功能扩大全局类模型。方法见[框架维护与扩展](framework-development.md)。
