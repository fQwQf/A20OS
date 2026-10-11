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
启动性能的实现与系统调用基准见 `docs/x86_64-cpu-id.md`。
