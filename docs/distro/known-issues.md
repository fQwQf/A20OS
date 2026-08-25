# 已知问题与排查笔记

桌面目前能起来、能渲染、键鼠可用。本文档记录每个已解决的问题（含根因与修法）和 尚存的小问题，供后续继续排查时参考。

## 一、输入：libinput 枚举不到设备（已解决）

### 现象（修复前）

桌面以 `WLR_LIBINPUT_NO_DEVICES=1` 起（无输入），libinput 的 udev 后端报 "no input devices"。

### 根因（两个独立的 ABI 缺口叠加）

1. **netlink uevent 没有 `SEQNUM=`**。eudev 的 `event_queue_insert` / `is_devpath_busy` 依赖每个事件有严格递增的 seqnum 排序。A20OS 之前不发 SEQNUM，所有事件的 seqnum 都是 0，于是**第一个入队的事件被误判为 "busy"**（`delaying_seqnum 0 == seqnum 0`），worker 永远不会 spawn，规则 不执行、db 只剩骨架。`udevadm test`（前台进程内跑规则）能出 ID_INPUT， 正是因为它不经过 worker 队列——之前因此误判成"udevd 运行期规则引擎坏了"。 修复：内核 netlink uevent 广播加全局递增的 `SEQNUM=`（`kernel/net/socket_netlink.c`）。

2. **`/dev/input/eventN` 与 `/sys/dev/char/<maj>:<min>` 的 syspath 不一致**。 libinput 的 `evdev_device_have_same_syspath()` 打开设备后，用 `udev_device_new_from_devnum()`（走 `/sys/dev/char/29:1`）创建的 syspath 与 enumerate（走 `/sys/class/input/event0`）的 syspath **逐字节比较**， 不等就拒收设备。Linux 里两者都是 symlink 到同一个 `/sys/devices/...` 真实路径；A20OS 之前没有统一路径。修复：
   - devfs 的 `/dev/input/` 动态列出并解析每个发布的 input class device （之前硬编码只有 event0），`/dev/input/event1+` 因此存在；
   - `/sys/dev/char/<maj>:<min>` 保持目录（libdrm 要 `device/drm` 子路径）， 同时 `readlink()` 返回**相对** class 路径（`../../class/input/event0`， 与 Linux 一致）；`vfs_readlinkat()` 允许带 readlink 实现的目录节点响应。 libudev 的 `util_resolve_sys_link()` 解析后 syspath 与 `/sys/class` enumerate 一致，设备被接受。

### 验证

`udevadm info --export-db` 显示 event0/event1 带 `E: ID_INPUT=1`、 `ID_INPUT_KEYBOARD=1`、`ID_INPUT_MOUSE=1`；labwc 日志出现 `Adding A20OS evdev mux` 和 `configuring input device A20OS evdev mux (event0/event1)`；会话不再需要 `WLR_LIBINPUT_NO_DEVICES`。

## 二、间歇性读路径自死锁（已解决）

### 现象（修复前）

桌面在高 I/O 阶段偶发整体挂死。串口 `LOCK-STALL` 显示 `owner==waiter` （同一 task）且 `owner_ra==waiter_ra`，即**同一把自旋锁被同一 task 递归获取**。 命中率约三到五成。

### 根因

`sys_read`/`sys_write` 持有 `vf->offset_lock`（mutex）时，直接向**用户页** 读写（`read_into_user` 用 `user_buffer_segment` 拿用户页 kaddr 交给 `vfs_read_file`）。若该用户页未映射，memcpy 在**持锁路径内**触发缺页；缺页 处理（写文件映射页等）可能重新进入同一条文件读路径，递归获取同一把锁—— mutex 内部的 spinlock 永久自旋。`addr2line` 把 ra 解析到 `read_into_user`/ `sys_read`（尾调用+内联掩盖了真正的持锁点），静态反查不到确切锁名，所以一直 是"间歇性死锁，抓不到现场"。

### 修复

两层修复：

1. **用户拷贝移出锁内**。`read_into_user` / `write_from_user` 对**所有**文件类型 统一经过内核 scratch buffer（`proc_scratch_buffer`）：先在持锁区内读/写到 内核缓冲，再在锁外 `copy_to_user`/`copy_from_user`，保证用户页缺页永远不在 持锁路径内发生。
2. **`mutex` 支持同 task 递归**。实测抓到递归点：一个文件系统的 read/write 路径（写回、`/proc/<pid>/fdinfo` 渲染等）在 `vf->offset_lock` 已被同 task 持有时会再次进入它——这是 offset_lock 的**合法重入**，不是竞争。`mutex_lock` / `mutex_unlock` 现在记录 per-owner depth，同 task 递归获取 depth+1 直接返回， 而不是在 mutex 内部的 spinlock 上自死锁。`procfs` 文件则完全跳过 offset_lock （`vfs_is_procfs_vfile`），因为渲染 `/proc/<pid>/fdinfo` 要锁别的 vfile 的 offset_lock，不能在持此锁时进行。

另外给 `spin_lock_at` 的 LOCK-STALL 报告加了 `owner==waiter` 时的 32 帧调用栈
+ IRQ 状态打印，递归 mutex 路径也短暂打印过命中次数，确保复现时可定位。

### 验证

修复前高负载下几乎每次必现 `LOCK-STALL`；修复后连续多次桌面 boot（含 I/O 压力 复现、完整 5 分钟窗口）均无 `LOCK-STALL`，桌面、输入、Xfconf 激活全部正常。

## 三、遗留小问题：dbus 同步调用偶发超时

会话里偶发 `dbus-update-activation-environment: ... NoReply: Did not receive a reply`，伴随 xfsettingsd/xfce4-panel 的 Xfconf CRITICAL（"Failed to initialize Xfconf: Timeout"）。xfconfd 通过 dbus activation 能激活（串口可见 `Successfully activated service 'org.xfce.Xfconf'`），但**某些 dbus 同步方法 调用会偶发超时**（约 30s 无回复），组件随后降级。不是每次必现。

怀疑方向：dbus 的某个机制（fd 传递、大消息、synchronous reply 与 event loop 的交互）在 A20OS 上偶发不完整。后续可给 dbus-daemon / xfconfd 加 debug，或 在内核侧观察 dbus 使用的 AF_UNIX SOCK_SEQPACKET 通道在大消息下的行为。

## 四、测试环境注意事项

- **rootfs 构建**：直接 `apk add` 走官方 CDN 会慢到像卡死；用预置的 `a20rootfs-builder` docker 镜像（apk 源已指 USTC），约 2 分钟一个镜像。 详见 [`build.md`](build.md)。
- **改 overlay 必须重建镜像**：debugfs 对 8GiB 的 metadata_csum ext4 打不开 rw，别想直接改镜像里的文件。
- **QEMU 串口日志在后台跑时的坑**：镜像被 QEMU 以 RW 打开，重复启动前要清掉 占用进程。判断占用者要看 `/proc/*/fd` 里谁持有 fat32.img/rootfs.img， **不要用 `pgrep -f`**——它会匹配到命令自己。
- **GUI 设备的 QEMU 参数**：桌面 boot 必须带 `-device virtio-keyboard-device,bus=virtio-mmio-bus.5 -device virtio-mouse-device,bus=virtio-mmio-bus.6 -device virtio-gpu-device,bus=virtio-mmio-bus.7`。缺了这些设备，内核没有 input/gpu class device，`/sys/class/input/` 为空，曾一度被误判成"sysfs 没建 input"。
- **宿主噪音**：宿主机会偶发刷 `Failed to write 'change' to '/sys/devices/.../uevent': Permission denied`，那是宿主 systemd-udevd 在 trigger 整机 /sys，跟 A20OS 无关。
