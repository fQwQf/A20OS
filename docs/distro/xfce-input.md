# XFCE 输入回归

物理 `/dev/input/eventN` 现在对应同名 input class 设备，每个打开的文件描述
拥有独立事件队列。旧的 `/dev/event0` 保留为聚合流。udev、libinput 和其他
探测程序同时打开设备时，不会再互相消耗事件。

设备名称、ID、按键、相对坐标、绝对坐标范围及 properties 来自驱动实际能力。
VirtIO 通过配置选择器查询能力，PCI transport 转发配置写入；USB HID 和
PS/2 提供各自的协议能力。共享层没有架构判断。内部查询不改变模块 vtable。

`/sys/class/input/eventN` 和 `/sys/dev/char/29:N` 是指向
`/sys/devices/virtual/input/eventN` 的相对符号链接，三条路径的 name、
properties 和 capabilities 一致。eudev 使用标准 `input_id` builtin 分类，
不再把所有节点强制标为键盘兼鼠标。

VirtIO 键盘还需要 `EVIOCGREP` 才能通过 libevdev 初始化。共享层报告当前
禁用的 evdev 软件重复状态（delay/period 均为 0），Wayland 合成器处理按键
重复。队列还实现 grab 冲突、revoke、时钟选择、按键/绝对坐标状态及
`SYN_DROPPED` 溢出恢复。

## 自动验证

```sh
make -j12 ARCH=x86_64 ABI=both NR_CPUS=4 CONFIG_KERNEL_PREEMPT=1 dev-build
python3 tools/tests/evdev-stream-smoke.py --arch x86_64
make -j12 ARCH=riscv64 ABI=both NR_CPUS=4 CONFIG_KERNEL_PREEMPT=1 dev-build
python3 tools/tests/evdev-stream-smoke.py --arch riscv64
```

两个测试均通过：物理设备能力、三条 sysfs 路径、两个独立读者和聚合流、
持键时打开新读者、grab/ungrab、revoke/poll HUP、溢出及状态恢复。
测试固定私有内核和 FAT 镜像副本，并通过串口 READY 标记同步 QEMU 输入。
RISC-V 的 MMIO slot 5 留给独立的用户态 uinputd 测试。

在已启动的 XFCE 会话内运行 `python3 tools/tests/xfce-input-smoke.py`，
检查真实 libevdev 初始化、udev 分类和 sysfs 能力一致性。这个测试脚本需要
复制到测试客体，不安装进生产镜像。

FAT32 小缓冲区 getdents 曾漏掉下一条目录记录，导致 xHCI 等模块未加载。
修复后 `vfs_stress --fat32-dir` 及 USB 键盘/鼠标中断输入 smoke 通过。
完整枚举也暴露了用户驱动控制台句柄错误：native FILE 句柄必须持有 vfile
引用，不能保存 Linux fd 数字。修复后四核 RISC-V 自动启动 uinputd、接收
按键、退出和关机通过。

## 实际桌面验证

QEMU GTK、4 CPU/4 GiB、virtio-gpu/virgl，使用生产 XFCE 镜像快照：

- 真实宿主鼠标移动、点击打开 Applications 菜单，并启动 Thunar。
- 拖动 Thunar 标题栏，窗口从约 `(120,117)` 移到 `(199,156)`。
- VirtIO 键盘通过 libevdev，Super+Return 打开终端。

本地证据位于 `build/browser-followup/`：`input-production-appmenu.png`、
`input-production-filemanager.png`、`input-drag-confirmed-{start,end}.png`、
`input-repeat-shortcut.png`，以及 `input-repeat-{x86,rv}.log`、
`input-usb-final.log`、`input-user-driver-auto.log`、`fatdir-smoke.log`。
启动性能的实现与系统调用基准见 `docs/kernel/arch/x86_64-cpu-id.md`。

## ext4 配置与存档完整性

进一步检查发现 ext4 的 `bg_itable_unused` 曾被写成 inode 的位图下标，
释放 inode 时还会错误地增大它，导致仍然存在的 inode 落入“未初始化”区间。
现在分配时使用 `组内有效 inode 数 - 下标 - 1`，释放时保留初始化高水位。
这同时保护桌面配置和 Minecraft 存档，不增加架构或应用例外。

`python3 tools/tests/ext4-inode-tail-smoke.py` 使用密集 inode 表，实际分配、
删除中间文件并复用 inode，关机后检查文件与宿主机 `e2fsck -fn`。
旧内核负对照失败，新内核通过；磁盘为私有副本且禁用 QEMU snapshot，
因此检查的是落盘后的元数据。重建的生产 XFCE 镜像也通过只读 fsck。

## 浏览器与游戏追加验收

Firefox 151.0.3 的 HTTP/HTTPS DOM、Bing 首页/搜索（10 条结果）、刷新和
5 分钟页面存活检查通过，见 `input-desktop-release-web.log`。

最终使用重建镜像、四核 KVM/virgl 的单 VM 验证 Minecraft 1.21.11 demo：

- 鼠标点击 Play Demo World、Continue Playing，进入世界。
- GLFW 建立相对指针与 pointer lock，非零 relative_motion 转动视角。
- 客户端收到 BTN_LEFT/BTN_RIGHT 的按下与释放；Esc 销毁 pointer lock。
- 鼠标保存回标题、正常退出（status=0），存档 gzip 检查与 sync 通过。

完整客户端 Wayland 跟踪和 QA 结果位于 `input-desktop-complete.log`。
截图为 `input-complete-mouse-play.png`、`input-complete-captured-before.png`、
`input-complete-look-around.png`、`input-complete-pause.png`、
`input-complete-saved-title-verified.png`、`input-complete-exited-desktop.png`。
本次使用快照，未重复“重启同一可写镜像后重载存档”的前期验收；音频仍为
OpenAL No Output，未验证实体声卡输出。

早期同时运行两个 virgl VM 时遇到过一次 GPU `cmd=202` 超时；该轮不计入
游戏输入验收。最终单 VM、重建镜像的完整游玩/保存/退出未复现，这不等于
已修复多 VM 并行下的图形超时。QA 脚本和 Wayland 调试均未进入生产镜像。
