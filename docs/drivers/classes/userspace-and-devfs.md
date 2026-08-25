# 用户接口与 devfs

A20OS 的 driver class 是内核内部接口，但 class publication 同时驱动动态 devfs/sysfs 视图。char、block、audio 自动生成通用节点；net、input、display 不自动生成同名动态 devfs 节点，分别由内核消费者或既有聚合 ABI 使用。

## 当前数据路径

```text
userspace read/ioctl
  -> VFS vfile_ops
  -> devfs 动态 class 节点或固定聚合节点
  -> class_device_t 引用和 online 检查
  -> typed class_ops(device_t *, kernel buffer)
  -> drv_priv / hardware
```

`kernel/fs/devfs/devfs.c` 的 `g_nodes[]` 仍定义 `/dev/fb0`、`/dev/event0`、tty、loop 等固定服务节点。除此之外，根目录 lookup/readdir 会查询 class registry，为在线的 char、block、audio 实例动态构造 `/dev/charN`、`/dev/diskN`、`/dev/audioN`。打开文件持有 `class_device_t` 引用；remove 把它置为 offline 并排空在途调用，后续操作返回 `-ENODEV`。

所有 class 还动态出现在 `/sys/class/{char,block,net,input,display,audio}/<name>/dev`；`dev` 内容是该 class device 的 major:minor。既有 `/sys/class/drm` 和 `/sys/block/loopN` 兼容视图独立保留。

## 用户 buffer 边界

read/write 的 VFS/ABI 路径负责用户地址检查和复制，class ops 接收内核 buffer。ioctl 尚未完全统一：dynamic block 的 `GET_CAPACITY`/`GET_SECTOR_SZ` 和 audio 通用 ioctl 在适配器中 usercopy；未知 block ioctl 与 char ioctl 当前把原始 `arg` 继续传给 class op。新增自定义 ioctl 不能假定该指针已验证，也不能在硬件驱动中直接解引用；应先补通用封送层。这个已知边界不改变 read/write 的内核 buffer 契约。

## 新节点怎么做

当前没有让硬件驱动自选任意节点名的 `devfs_register_device(name, dev, ops)` API。不要从硬件驱动直接修改 `g_nodes` 或持有 vnode。选择顺序应是：

1. char、block、audio 注册 class 后直接获得动态通用节点。
2. input/display 注册 class 后由固定 `/dev/event0`、`/dev/fb0` 聚合或选择。
3. 若是内核子系统消费者，例如 network，注册 class 后由 lwIP 消费，不需要 `/dev`。
4. 若确实需要新通用用户接口，先设计 class 与 ABI，再在 devfs 增加与硬件无关的适配器。
5. 单一厂商调试接口应优先放 debug/proc 通道，不要把不稳定寄存器操作发布为长期 ioctl ABI。

新增 devfs ABI 必须规定节点命名、多实例编号、open 时如何绑定 `device_t`、权限、read/write 单位、阻塞、poll、ioctl 结构版本和 remove 后已打开 fd 的行为。

## Display

`/dev/fb0` 的 open 当前不绑定独立引用，而每次 ioctl 获取默认 display。驱动 probe 需 `gpu_device_register`，remove 需 unregister。映射由 framebuffer 适配层完成，驱动只报告物理 backing 和刷新操作。

## Input

`/dev/event0` 是聚合器，不是严格的一设备一节点 evdev。它枚举 `DEV_CLASS_INPUT`（例如 xHCI）并合并 VirtIO input 事件。新 input 类驱动实现 `read/poll` 即可被发现。事件必须是完整 `struct input_event`；class read 在无事件时返回 `-EAGAIN`，不得让适配器依赖硬件私有 getter。

## Audio

每个 `DEV_CLASS_AUDIO` 实例自动发布一个 `/dev/audioN`。devfs 只负责断开检查、用户 buffer 转换，并把通用 ioctl/close 交给 `audio_core`；它不包含 HDA、virtio-sound、PC Speaker 或其他硬件策略。客户端必须先查询 capability；PCM fd 的正常 close 会先 drain，立即中止则使用 STOP。完整 ABI 见 [音频子系统](audio.md)。

## Block 与 network

network 由 `lwip_stack.c` 枚举 `DEV_CLASS_NET`，没有网卡字符节点。每个 block class 实例有动态 `/dev/diskN`，文件系统也可直接枚举 class 接入挂载请求；驱动只实现 class ops，不创建私有节点或 getter。

## ABI 设计检查

- 用户结构使用固定宽度整数，不使用裸指针或随架构变化的 `long`，除非已是兼容 ABI。
- 结构带 `size/version` 或预留字段，输出先清零，防止泄漏内核栈。
- 所有长度、offset、count 做溢出检查；未知 flags 拒绝。
- 非阻塞语义统一用 `-EAGAIN`，未知 ioctl 用 `-ENOTTY`。
- remove 后操作返回 `-ENODEV`，不得访问已释放 `drv_priv`。
- 用户映射必须定义 cache 属性、共享/复制语义、fork 行为和撤销策略。

不要这样做：从具体硬件驱动里直接创建或修改 devfs 节点。这会让节点生命周期和硬件 remove 脱节，open 的 fd 可能在设备释放后仍然指向已释放 `drv_priv`，造成 use-after-free。
