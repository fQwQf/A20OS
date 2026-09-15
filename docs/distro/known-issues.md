# 已知问题与排查笔记

桌面目前能起来、能渲染、键鼠可用。本文档只记录**当前状态与排查提示**（症状、是否已解决、去哪看），不记录实现过程。

## 一、已解决

### 输入：libinput 枚举不到设备
- 症状：libinput udev 后端报 "no input devices"，桌面无输入。
- 提示：两处 ABI 缺口——netlink uevent 缺 `SEQNUM=`（`kernel/net/socket_netlink.c`）；`/dev/input/eventN` 与 `/sys/dev/char/<maj>:<min>` 的 syspath 不一致（devfs + sysfs）。

### 间歇性读路径自死锁
- 症状：高 I/O 阶段偶发整体挂死，串口 `LOCK-STALL` 显示 `owner==waiter`。
- 提示：用户拷贝不得在 `vf->offset_lock` 持锁区内触发缺页；`mutex` 需支持同 task 递归。

### 高 CPU / 桌面空转
- 症状：桌面 idle 时宿主 CPU 100%；组件事件循环空转。
- 提示：三处——x86_64 空闲循环要 HLT；`epoll_ctl` 要按 Linux `file_can_poll()` 拒绝不可 poll 的 fd；DRM/input/epoll 要提供 `poll_sources`，否则 readiness 每 1ms 兜底唤醒。

### x86_64 桌面组件随机 SIGSEGV
- 症状：xfdesktop/panel/thunar/gst 等随机野指针崩溃，仅 x86_64。
- 提示：x86_64 要在上下文切换与信号帧保存/恢复 FPU/SSE。对照 aarch64/riscv64/loongarch64 的 trap 帧已保存 SIMD——缺这层时被抢占线程的 XMM 会被下一个任务覆盖。

### thunar 运行一段时间后崩溃
- 症状：GDK 报 `Truncating shared memory file failed: Out of memory`，随后 libwayland 空指针崩溃。
- 提示：wl_shm 池是 memfd，其数据曾用单个连续 kmalloc 缓冲（1024x768x4 需 order-10 连续块），内存碎片化后 ftruncate 失败。现改为按需 order-0 页数组（`kernel/fs/memfd.c`）；页缓存写回/回收仍可继续观察。

### elogind 启动失败
- 症状：`elogind-daemon: Failed to open pin file` 紧接 `Failed to allocate manager object`。
- 提示：elogind 在 manager setup 阶段 pin 自己的 cgroup，路径取自 `/proc/1/cgroup`（`init.scope`）。需先建好该目录与 `/run/elogind`；用**普通 `mkdir`**（busybox `mkdir -p` 会误判 sysfs 父目录为「Not a directory」）。

### thunar 缩略图服务缺失
- 症状：`Thumbnailer1 was not provided by any .service files`，并反复 `Thumbnailer Proxy Failed ... re-initialize`。
- 提示：world 清单补 `tumbler`（提供 `org.freedesktop.thumbnails.Thumbnailer1`）。

### 桌面没有壁纸
- 提示：`xfdesktop` 包自带 `/usr/share/backgrounds/xfce/*`；backdrop 属性路径是 `/backdrop/screen0/monitor<id>/workspace0/last-image`，其中 `<id>` 是 libxfce4windowing 用 make/model/serial/connector 计算的 **SHA1**（不是固定名字），配置时需按实际显示器标识写入。

## 二、未解决

### 点击异常：下拉菜单项不启动应用、窗口控制键无效
- 症状：指针能动、能点；面板按钮有响应（能开菜单），但下拉菜单项不启动应用，窗口控制键无响应。
- 现状（在 `e2fsck` 干净的 rootfs 上复验）：指针移动与坐标正常——悬停分类项会弹出子菜单；点击面板按钮会打印 `press on layer-(sub)surface`。但点击子菜单项既不启动应用（无新 `xdg_surface`、无窗口），也不关菜单；落在子菜单上的按下**不打印任何 labwc 日志**，而面板按钮/分类项会打印 `press on layer-(sub)surface`。
- 已排除：内核输入通路本身。`struct input_event` ABI、PS/2 按键解码、每包 `SYN_REPORT`、evdev 打戳、内核与 vDSO 的 CLOCK_MONOTONIC 时基逐项核对一致；libinput 也枚举到了设备（`Adding A20OS evdev mux [0:0]`）。时间戳（CLOCK_MONOTONIC）曾是一处真实成因，但修好后症状仍在。
- 提示：是否打日志取决于命中节点的类型（只有层表面分支会打印）。下一步应在 guest 内直接看 `wlr_scene_node_at` 的命中结果与客户端实际收到的 `wl_pointer.button`，不要再从截图推断。

### dbus 同步调用超时 / 对端 pid 为 -1
- 症状：会话里 `NoReply`，xfsettingsd/panel 报 Xfconf/dconf 超时后降级；dbus 日志里身份异常：`dbus-daemon[44]: [session uid=0 pid=18446744073709551615 pidfd=5] ...`（pid 为 -1）。
- 现状：在 `e2fsck` 干净的 rootfs 上稳定复现。同一行里 `requested by ':1.7' (uid=0 pid=92 comm=".../xfsettingsd")` 的 pid 却是对的。
- 已定位：`kernel/net/socket_unix.c` 的 connect 路径把**发起端自己**的 pid 写进了 `s->peer_pid`（`s->peer_pid = cur->pid`），而发起端的对端应是服务端；服务端（accept 出的子 socket）那一路是对的。
- 已排除：`peer_pid` 未初始化（`net_socket_alloc()` 用 `obj_cache_alloc_zero()`）；`getpid()` 本身（`dbus-daemon[44]` 说明任务 pid 正确）。
- 提示：还需确认 dbus 这个 `pid=` 究竟取自 `SO_PEERCRED` 还是它自身身份；本机取不到 dbus 源码，guest 也没有 getty 可直接验证。

### 其他架构的 FPU/上下文
- 状态：riscv32、ppc64le 已补上 trap 帧的 FP 保存（ppc64le 仅编译验证）；**arm32 仍未修**（本机无 arm 工具链，且其 trap 帧与 trap.S 用逐字段断言强绑定，改动未经验证）。
- 提示：判定标准是「用户态是否启用 FP/SIMD」——arm32（`-mfpu=vfpv3-d16 -mfloat-abi=hard`）、riscv32（`ilp32d`）、ppc64le（`MSR_FP`）都启用；loongarch32/armv7m 无 FPU，不受影响。ppc64le 的向量保存受 `MSR_VEC` 门控而该位未设，需另存 FPR。

### 32 位架构构建
- 状态：riscv32 在 HEAD 上有多处既有编译错误（`arch_vdso_counter` 声明、`pfa_range_t` 断言、`proc.c` type-limits 等）；前两处已修，`proc.c` 等仍待处理。
- 提示：非 VDSO 架构（riscv32/arm32/loongarch32）都会撞到 `arch_vdso_counter` 声明缺失。

## 三、测试环境注意事项

- **测试前先 `e2fsck -fn` 校验镜像**：损坏的 rootfs 会**伪造出内核 bug**。实测一个报 `Block bitmap checksum does not match`、`Entry 'sys' ... unused inodes area` 的镜像，会让 6 个 udev worker 全部 `timeout; kill it`；换干净镜像后为 0。损坏镜像还会造成 `failed to create cairo scaled font`、应用起不来等假象。**新 `image-world` 产物 `e2fsck -fn` 是干净的**（5 个 pass 全过），说明损坏不是构建期引入、而是之后发生，具体来源未定位。
- **rootfs 构建**：直接 `apk add` 走官方 CDN 会慢到像卡死；用预置的 `a20rootfs-builder` docker 镜像（apk 源已指 USTC），约 2 分钟一个镜像。详见 [`build.md`](build.md)。
- **改 overlay 必须重建镜像**：debugfs 对 8GiB 的 metadata_csum ext4 打不开 rw。
- **QEMU 串口日志在后台跑时的坑**：镜像被 QEMU 以 RW 打开，重复启动前要清掉占用进程；判断占用者看 `/proc/*/fd`，不要用 `pgrep -f`。
- **GUI 设备的 QEMU 参数**：桌面 boot 需带 virtio keyboard/mouse/gpu 设备，否则没有 input/gpu class device。
- **宿主噪音**：宿主 systemd-udevd 偶发刷 `/sys/.../uevent: Permission denied`，与 A20OS 无关。
