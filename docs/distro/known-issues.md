# 已知问题与排查笔记

桌面现在能起来、能渲染，但还差两件事才算"完全可用"：键鼠（输入）和一个偶发的 死锁。两个都做了大量排查，本文档把已经确认的结论、还没解决的点、以及下一步 怎么走写清楚，省得下次从零开始。

## 一、输入：libinput 枚举不到设备

### 现象

桌面以 `WLR_LIBINPUT_NO_DEVICES=1` 起（无输入），因为 libinput 的 udev 后端 报 "no input devices"。

### 已经做到哪一步

整个 /sys 到 udev 数据库的链条大部分已经打通：

- `/sys/devices/virtual/input/event0`、`/sys/class/input/event0/{uevent,dev,subsystem}` 都在（提交 `693282f`、`2ea932a2`）；
- 输入 class device 的 devt 已经对齐 devfs 节点：`/dev/input/event0` 是 29:1， `class_devt` 对 INPUT 返回 `0x1d01+index`，udev 数据库里也是 MAJOR=29 MINOR=1， 不再对不上；
- netlink uevent 广播、udevd bind 时的整组重放、sysfs uevent 文件可写（冷插拔） 都验证过——串口能看到 `[UEVENT] add input/event0 -> delivered=1`，说明 udevd 确实收到了；
- 规则 `usr/lib/udev/rules.d/99-a20-input.rules`（`SUBSYSTEM=="input"` → `ID_INPUT` / `ID_INPUT_KEYBOARD` / `ID_INPUT_MOUSE`）本身没问题： `udevadm test /class/input/event0` 能正确产出这三个变量。

### 卡点：udevd 运行期不跑规则引擎

`udevadm test` 是进程内模拟，能跑出 ID_INPUT。但 udevd **运行期**收到 uevent、 也建了数据库条目，条目却只是骨架——只有 uevent 文件自带的 MAJOR/MINOR/DEVNAME/SUBSYSTEM/DEVPATH，没有任何规则产物（连 50-udev-default 的 TAGS 都没有）。也就是说规则引擎在 daemon 的运行时处理路径上没有真正执行。

eudev 的 udevd 用 worker 子进程处理事件再回写数据库，所以下一步的第一怀疑对象 是 worker 路径：fork/exec 是否失败、事件处理是否根本没走到规则、数据库写入是否 被某个 syscall 卡住。给 udevd 加 `-d`/`--debug`，盯 `/run/udev/data/` 是否出现 规则产物，应该能定位到具体是哪一步断了。

### 备选路线

- 如果确认是 worker 机制在 A20OS 上有问题，可以看看 wlroots/labwc 是否暴露 libinput 的 path 后端开关，直接指定设备文件绕过 udev；
- 或者先在 init 里直接往 `/run/udev/data/` 写带 ID_INPUT 的条目，仅作验证 "udev 数据库有 ID_INPUT 之后 libinput 就认"这一环。

验证信号：`udevadm info --export-db | grep -A15 'input/event0'` 里出现 `E: ID_INPUT=1`；然后去掉 `start-xfce4-session` 里的 `WLR_LIBINPUT_NO_DEVICES=1`，labwc 日志出现 "New input device"。

## 二、偶发死锁：读路径上的自死锁

### 现象

桌面在高 I/O 阶段偶尔整个挂死。串口的 LOCK-STALL 显示 `owner==waiter`（同一个 task）且 `owner_ra==waiter_ra`，也就是**同一把自旋锁被 同一个任务递归获取**。命中率大概三到五成，属于间歇性，最难抓的那种。

### 已经确认的锁特征

- 锁在堆上（slab 分配，周围能看到 0xcafebabe canary）；
- 不是 `spin_init` 注册的锁——是某个结构体里零初始化/复合初始化的自旋锁字段， 所以常规的"给锁起名"调试手段对它无效；
- 字段偏移约 +0x18；
- `addr2line`/`kallsyms` 把 ra 解析到 `read_into_user`/`vfs_read_file` 一带， 但那是尾调用（`vfs_read_file` 尾部 `return vf->ops->read(...)`），真正的持锁点 在尾调用的 fs/缓存读取里，被内联掩盖，静态反查不到确切那把锁。

已排除的候选：page_cache 全局锁、bcache、mm->lock、fdtable files->lock、 proc_lock（这些要么已命名、要么在 .data 段，地址都对不上）。

### 下一步

1. 在 `spin_lock_at` 入口加一句自检：`lock->locked && lock->owner == cur` 说明同一 任务在重复获取，立刻把 lock 地址、`owner_ra`、本次 `waiter_ra`（都过 `kallsyms_lookup`）打出来。这是唯一能稳定抓到递归点的手段，代价是要多跑几次。
2. 抓到递归点之后，修法通常是"持锁期间不做可能缺页/嵌套进同一路径的操作"： 读路径先把数据读进内核 bounce buffer，再在锁外 copy_to_user。
3. 这个自检建议做成受 `CONFIG_DEBUG_LOCKS` 控制的常驻检测，而不是一次性 printk。

## 三、测试环境注意事项

- **rootfs 构建**：直接 `apk add` 走官方 CDN 会慢到像卡死；用预置的 `a20rootfs-builder` docker 镜像（apk 源已指 USTC），约 2 分钟一个镜像。 详见 [`build.md`](build.md)。
- **改 overlay 必须重建镜像**：debugfs 对 8GiB 的 metadata_csum ext4 打不开 rw， 别想直接改镜像里的文件。
- **QEMU 串口日志在后台跑时的坑**：镜像被 QEMU 以 RW 打开，重复启动前要清掉 占用进程。判断占用者要看 `/proc/*/fd` 里谁持有 fat32.img/rootfs.img， **不要用 `pgrep -f`**——它会匹配到 grep 命令自己。
- **宿主噪音**：宿主机会偶发刷 `Failed to write 'change' to '/sys/devices/.../uevent': Permission denied`，那是宿主 systemd-udevd 在 trigger 整机 /sys，跟 A20OS 无关。
- 排查期间曾遇到 main 上有并行内核开发（userfaultfd/perf/PI-futex/native-ABI pager），工作区一度处于不可编译、启动偶发挂死的中间态，当时误判成"distro 启动被改挂了"。事后用更长的验证窗确认那是间歇死锁加 WIP 中间态的叠加， 并行提交本身能进桌面。
