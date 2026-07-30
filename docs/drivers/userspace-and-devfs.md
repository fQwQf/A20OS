# 用户接口与 devfs

A20OS 的 driver class 是内核内部接口。注册类设备不会自动生成任意 `/dev` 节点。一个 probe 成功的驱动仍可能没有用户可见入口，外部开发者必须理解这一点。

## 当前数据路径

```text
userspace read/ioctl
  -> VFS vfile_ops
  -> devfs 固定节点适配器
  -> device_find_by_class() / class registry
  -> typed class_ops(device_t *, kernel buffer)
  -> drv_priv / hardware
```

devfs 当前在 `kernel/fs/devfs.c` 的静态 `g_nodes[]` 中定义节点。通用固定节点包括 `/dev/fb0` 和 `/dev/event0`。display 使用 `gpu_device_get_default()`；input 适配器枚举 `DEV_CLASS_INPUT` 并聚合事件。

## 用户 buffer 边界

VFS/ABI 层负责用户地址检查和 `copy_to_user/copy_from_user`。class ops 接收内核 buffer，因此驱动不得调用用户 copy，也不得把 class buffer 当用户地址。ioctl 结构先在适配器中复制到内核栈/堆、验证，再调用 class op。

## 新节点怎么做

当前没有稳定的 `devfs_register_device(name, dev, ops)` API。不要从硬件驱动直接修改 `g_nodes` 或持有 vnode。选择顺序应是：

1. 若已有设备类和固定适配器，注册 class 即可，例如 input/display。
2. 若是内核子系统消费者，例如 network，注册 class 后由 lwIP 消费，不需要 `/dev`。
3. 若确实需要新通用用户接口，先设计 class 与 ABI，再在 devfs 增加与硬件无关的适配器。
4. 单一厂商调试接口应优先放 debug/proc 通道，不要把不稳定寄存器操作发布为长期 ioctl ABI。

新增 devfs ABI 必须规定节点命名、多实例编号、open 时如何绑定 `device_t`、权限、read/write 单位、阻塞、poll、ioctl 结构版本和 remove 后已打开 fd 的行为。

## Display

`/dev/fb0` 的 open 当前不绑定独立引用，而每次 ioctl 获取默认 display。驱动 probe 需 `gpu_device_register`，remove 需 unregister。映射由 framebuffer 适配层完成，驱动只报告物理 backing 和刷新操作。

## Input

`/dev/event0` 是聚合器，不是严格的一设备一节点 evdev。它枚举 `DEV_CLASS_INPUT`（例如 xHCI）并合并 VirtIO input 事件。新 input 类驱动实现 `read/poll` 即可被发现。事件必须是完整 `struct input_event`；class read 在无事件时返回 `-EAGAIN`，不得让适配器依赖硬件私有 getter。

## Audio

每个 `DEV_CLASS_AUDIO` 实例自动发布一个 `/dev/audioN`。devfs 只负责断开检查、用户 buffer 转换和 class op 转发，不应包含 HDA、PC Speaker 或其他硬件策略。客户端必须先查询 capability；PCM fd 的正常 close 会调用驱动 close 操作完成 drain，立即中止则使用 STOP。完整 ABI 见 [音频与 Intel HDA](audio.md)。

## Block 与 network

network 由 `lwip_stack.c` 枚举 `DEV_CLASS_NET`，没有网卡字符节点。文件系统适配器负责把 `DEV_CLASS_BLOCK` 接入挂载请求；驱动只实现 class ops，不创建挂载节点或私有 getter。

## ABI 设计检查

- 用户结构使用固定宽度整数，不使用裸指针或随架构变化的 `long`，除非已是兼容 ABI。
- 结构带 `size/version` 或预留字段，输出先清零，防止泄漏内核栈。
- 所有长度、offset、count 做溢出检查；未知 flags 拒绝。
- 非阻塞语义统一用 `-EAGAIN`，未知 ioctl 用 `-ENOTTY`。
- remove 后操作返回 `-ENODEV`，不得访问已释放 `drv_priv`。
- 用户映射必须定义 cache 属性、共享/复制语义、fork 行为和撤销策略。

 不要这样做：从具体硬件驱动里直接创建或修改 devfs 节点。这会让节点生命周期和硬件 remove 脱节， open 的 fd 可能在设备释放后仍然指向已释放 `drv_priv`，造成 use-after-free。
