# 发行版对内核提出了哪些要求

distro 路径跑的是原生 Alpine，内核没有"改自研组件绕过去"的自由。wlroots/labwc、 libinput、libdrm、eudev 这些上游组件会按 Linux 的标准行为去探测设备，缺一样就 起不来。这份文档把这些要求按子系统列一遍，注明内核在哪里满足。

## Linux ABI 与进程模型

Alpine 的二进制全部是 musl 动态链接，所以第一条硬要求就是 `exec` 能正确处理 shebang、ELF interpreter（`/lib/ld-musl-riscv64.so.1`），mmap 支持 MAP_PRIVATE/MAP_FIXED，TLS（`tp` 寄存器）能正确建立。这块是整个桌面能起来的 地基，也是当初踩坑最多的地方——一个 musl 加载期空指针崩溃的问题就是从 `exec`/mmap 语义不对引出来的。

## /dev（devfs）

内核的 devfs（`kernel/fs/devfs/devfs.c`）静态提供这些节点：

- `/dev/dri/card0`：DRM 设备（virtio-gpu）；
- `/dev/input/event0`：输入复用器，同时挂一个 `/dev/input/event0` 别名 （libseat/seatd 只认 `/dev/input/event*`）；
- 以及 loop、ptmx/pts、tty 等基础节点。

一个已知的不完全点：udevd 会报 `failed to chmod '/dev/loop-control'` 和 `failed to apply permissions on static device nodes`——devfs 不支持按 udev 规则 改权限，报错但不致命。

## /proc 与 /sys

sysfs 是精简实现（`kernel/fs/sysfs.c`），但发行版需要的东西都补上了：

- `/sys/class/drm/card0`，含 `card0-Virtual-1/{enabled,status,modes}` 和 `device/modalias`；
- `/sys/class/input/event0` / `event1`，含 `dev`、`uevent`、`subsystem` 符号链接；
- `/sys/dev/char/<maj>:<min>`，含 `uevent`、`device`——libdrm 靠它识别 DRM 设备；
- `/sys/devices/virtual/<subsys>/<name>`：udev 收到 uevent 后按 DEVPATH 来这里找设备。

设备热插拔走 netlink **KOBJECT_UEVENT**（`kernel/net/socket_netlink.c` + `kernel/drivers/core/driver_class.c`）：设备发布时广播 add uevent；udevd 的 netlink socket bind 时把当前设备集重放一遍；sysfs 的 uevent 文件做成可写，让 `udevadm trigger` 的冷插拔路径能走通。调试时在串口里能看到的 `[UEVENT] add input/event0 -> delivered=1` 就是这条广播的痕迹。

## DRM

wlroots 探测显卡的流程极其严格，下面每一条都是实测中卡过、然后在内核补齐的：

- **唯一 KMS 对象 ID**。plane/crtc/connector/encoder 的 ID 必须互不相同 （connector=1/encoder=2/crtc=3/plane=4）。wlroots 的 `get_drm_prop` 用 `DRM_MODE_OBJECT_ANY` 只按 ID 反查对象属性，之前全部用 1 导致它查 plane 时拿到 connector 的 EDID，后端创建直接失败。
- **`drm_mode_get_plane` 必须是标准 32 字节 UAPI 结构**。曾给它多塞了三个非 UAPI 字段，`copy_to_user` 按 44 字节写回，溢出 libdrm 栈上的结构、踩掉 canary， labwc 一启动就在 `drmModeGetPlane` 里 `__stack_chk_fail`。plane 的 type 要 通过属性机制（`OBJ_GETPROPERTIES`/`GETPROPERTY` 里的 "type"）暴露，不能塞进 结构体。
- **能力位与属性**。`DRM_CAP_PRIME`、`SET_CLIENT_CAP` 的 universal planes （cap 2/3），plane 的 `type=PRIMARY` 属性，以及连接器的 EDID blob 属性。
- **页翻转事件语义**。wlroots 依赖 flip 完成事件推进帧调度，事件丢一个就可能 永久卡死。当前实现是每 fd 一个固定 FIFO、seq 单调、时间戳用单调钟、pending flip 未完成时再 flip 返回 EBUSY。

## 输入 / evdev

`/dev/input/event0` 提供完整的 evdev ioctl 面（`EVIOCGVERSION`/`EVIOCGID`/ `EVIOCGBIT`/`EVIOCGNAME`/`EVIOCGKEY`/`EVIOCGABS` 等， `kernel/drivers/input/input_mux.c`），这样 udev 的 input_id 和 libinput 才能读到 设备能力位去分类设备。输入设备作为 class device 发布时，其 devt 要和 devfs 节点 一致（`/dev/input/event0` = 29:1），否则 udev 数据库里的设备对不上实际节点。

## 其它零零碎碎

- `PR_SET_PDEATHSIG` / `PR_GET_PDEATHSIG`（elogind 需要，`kernel/abi/linux/sys_proc.c`、 `kernel/proc/exit.c`）。
- cgroup：elogind 想挂 `/sys/fs/cgroup`，内核目前没有，挂载失败但非致命。
