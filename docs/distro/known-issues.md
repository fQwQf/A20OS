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

### 点击异常：下拉菜单项不启动应用、窗口控制键无效
- 症状：指针能动、能点，但菜单项不启动应用，最小化/最大化/关闭无响应。
- 提示：evdev 事件时间戳必须是 CLOCK_MONOTONIC。PS/2 与 USB-HID 驱动曾填 0，libinput 因此报 ~130s 处理延迟并把按键去抖定时器排在过去；由 `kernel/drivers/input/input_mux.c` 在交给用户态前统一打戳。

### thunar 运行一段时间后崩溃
- 症状：GDK 报 `Truncating shared memory file failed: Out of memory`，随后 libwayland 空指针崩溃。
- 提示：wl_shm 池是 memfd，其数据曾用单个连续 kmalloc 缓冲（1024x768x4 需 order-10 连续块），内存碎片化后 ftruncate 失败。现改为按需 order-0 页数组（`kernel/fs/memfd.c`）；页缓存写回/回收仍可继续观察。

## 二、未解决

### dbus 同步调用偶发超时
- 症状：会话里偶发 `NoReply`，xfsettingsd/panel 报 Xfconf 超时后降级；非必现。
- 提示：怀疑 dbus 的 fd 传递/大消息/同步回复与事件循环的交互；可给 dbus-daemon/xfconfd 加 debug，或观察内核侧 AF_UNIX SOCK_SEQPACKET 大消息行为。

### 其他架构的 FPU/上下文
- 状态：riscv32、ppc64le 已补上 trap 帧的 FP 保存（ppc64le 仅编译验证）；**arm32 仍未修**（本机无 arm 工具链，且其 trap 帧与 trap.S 用逐字段断言强绑定，改动未经验证）。
- 提示：判定标准是「用户态是否启用 FP/SIMD」——arm32（`-mfpu=vfpv3-d16 -mfloat-abi=hard`）、riscv32（`ilp32d`）、ppc64le（`MSR_FP`）都启用；loongarch32/armv7m 无 FPU，不受影响。ppc64le 的向量保存受 `MSR_VEC` 门控而该位未设，需另存 FPR。

### 32 位架构构建
- 状态：riscv32 在 HEAD 上有多处既有编译错误（`arch_vdso_counter` 声明、`pfa_range_t` 断言、`proc.c` type-limits 等）；前两处已修，`proc.c` 等仍待处理。
- 提示：非 VDSO 架构（riscv32/arm32/loongarch32）都会撞到 `arch_vdso_counter` 声明缺失。

## 三、测试环境注意事项

- **rootfs 构建**：直接 `apk add` 走官方 CDN 会慢到像卡死；用预置的 `a20rootfs-builder` docker 镜像（apk 源已指 USTC），约 2 分钟一个镜像。详见 [`build.md`](build.md)。
- **改 overlay 必须重建镜像**：debugfs 对 8GiB 的 metadata_csum ext4 打不开 rw。
- **QEMU 串口日志在后台跑时的坑**：镜像被 QEMU 以 RW 打开，重复启动前要清掉占用进程；判断占用者看 `/proc/*/fd`，不要用 `pgrep -f`。
- **GUI 设备的 QEMU 参数**：桌面 boot 需带 virtio keyboard/mouse/gpu 设备，否则没有 input/gpu class device。
- **宿主噪音**：宿主 systemd-udevd 偶发刷 `/sys/.../uevent: Permission denied`，与 A20OS 无关。
