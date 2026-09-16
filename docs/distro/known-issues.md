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

### overlay 配置：桌面图标、账号库、polkit 规则权限
- 桌面图标：`xfce4-desktop.xml` 里 `desktop-icons/style` 写成了 `1`（`XFCE_DESKTOP_ICON_STYLE_WINDOWS`＝「最小化窗口图标」），而同一文件又配了 `file-icons`（只有 style `2` 才生效）。改成 `2` 后桌面出现 Home/Trash/Filesystem 图标。
- 账号库：overlay 的 `etc/passwd` / `etc/group` 在 `apk add` **之前**落地，而 xfce world 不含 `alpine-base`，所以它就是账号库基底。原文件是 Debian 风格的极简集合（缺 `nobody`/`daemon`/`wheel`/`games`…，且 `audio=29`/`video=44`/`kvm=47`/`input=290` 等 gid 与 Alpine 不符）。已按 `alpine-baselayout-data` 的规范集合重写（仅 root 的 shell 保留 `/bin/zsh`）；内核 devfs 的设备节点 gid 恒为 0，不依赖这些 gid。
- polkit：Alpine 的 polkit 包把 `/etc/polkit-1/rules.d/`、`/usr/share/polkit-1/rules.d/50-default.rules` 打成 `0700 polkitd:polkitd`；`mkrootfs --usermode` 无法 chown，镜像里变成 `root:root 0700`，polkitd 读不到 → 会话日志出现 `Permission denied` + `Error loading script .../50-default.rules`。`mkrootfs` 现在在 mkfs 的 fakeroot 会话里按 apk 归档声明的 uname/gname 补回属主，会话日志已无该错误。
- 死配置：删除了 X11 时代的 `packages/overlay/xfce-x86_64/` + `packages/world/xfce-x86_64.world`（`instances/xfce-x86_64.toml` 用的是共享的 `xfce` world），以及 devtools overlay 里无人引用的 `poweroff-helper`。
- 两条 XFCE 路径（`tools/a20 run xfce-*` 与 `make distro-run`）原本各带一份 rc.xml/session.conf/udev 规则；现在 `user/rootfs/alpine/build.sh` 也应用 `packages/overlay/xfce`（跳过其 `etc/`），会话层单一来源。

### 点击异常：窗口控制键无效、点击不能聚焦
- 症状：指针能动、面板按钮有响应，但窗口标题栏的最小化/最大化/关闭无响应，点击窗口本体不能聚焦/提升，标题栏也不能拖动。
- 根因：**overlay 的 `rc.xml` 把 labwc 的内置默认绑定整表顶掉了**。labwc 只读一个 `rc.xml`（XDG 顺序里的第一个，即 `/root/.config/labwc/rc.xml`），**不合并** `/etc/xdg/labwc/rc.xml`；而它的内置默认 key/mouse 绑定只在解析结果里 `rc.keybinds` / `rc.mousebinds` **为空**时才安装（`src/config/rcxml.c` 的 `post_processing()`）。overlay 里写了 5 个 keybind 和 1 个 `Root` mousebind，于是 `rc.mousebinds` 非空 → 32 条默认鼠标绑定（`Client/Titlebar/Border Left Press → Focus+Raise`、`Title Left Drag → Move`、`Close/Iconify/Maximize Left Click → Close/Iconify/ToggleMaximize`……）一条都没装；keybind 同理，Alt-Tab/Alt-F4 也一起丢失。面板不受影响，因为它的按钮是客户端 GTK 自绘，labwc 只要有 `ctx.surface` 就照常转发指针事件，与 mousebind 无关。
- 修法：在 overlay 的 `<keyboard>` / `<mouse>` 里显式写 `<default />` 拉回内置默认。必须放在**前面**：`deduplicate_*_bindings()` 保留**后**出现的定义，所以后面显式写的绑定（`W-Return` 等）仍然覆盖默认值。
- 验证：guest 日志从"只有 `create mousebind for Root`、没有 `load default mouse bindings`"变为 `Loaded 32 merged mousebinds` + `Replaced 1 mousebinds`（Root Right 与默认重复被去重）+ `Replaced 1 keybinds`（`W-Return` 覆盖默认的 `lab-sensible-terminal`）；桌面里左键点击 root 触发了 `Handling action 15: ShowMenu`——这条绑定只存在于被拉回的默认表里。
- 涉及文件：`packages/overlay/xfce/root/.config/labwc/rc.xml`（`tools/a20 run xfce-*`）与 `user/rootfs/alpine/overlay/root/.config/labwc/rc.xml`（`make distro-run`），两处同一缺陷。

## 二、未解决

### JVM：需要 `/usr/bin/java` 封装（exec_path 不解析符号链接 + 堆参数）
- 现象：`java -version` 报 `Error loading shared library libjli.so: No such file or directory (needed by java)`；直接用真实路径
  `/usr/lib/jvm/java-21-openjdk/bin/java -Xms256m -Xmx512m -version` 则正常输出 `openjdk version "21.0.12"`。
- 根因链（已核实）：`libjli.so` 在 `/usr/lib/jvm/java-21-openjdk/lib/`，`java` 二进制靠 RPATH `$ORIGIN:$ORIGIN/../lib` 找它；
  musl 的 ldso 用 `readlink("/proc/self/exe")` 展开 `$ORIGIN`（`user/external/musl/ldso/dynlink.c` 的 `fixup_rpath()`）；
  而 A20OS 的 `/proc/<pid>/exe` 原本是**普通文件**（`readlink` 返回 `-EINVAL`），且 `exec_path` 只做「绝对化 + 归一化」、
  不解析符号链接（`kernel/proc/exec.c:846` 起），于是 `$ORIGIN` 停在 `/usr/bin`。
- 已修：`/proc/<pid>/exe` 与 `/proc/<pid>/cwd` 现在是 magic symlink（`kernel/fs/procfs/procfs.c`：vnode 类型 +
  `procfs_readlink`）。实测 `ls -l /proc/self/exe` 已显示 `-> /bin/ls`。
- 仍未修：`exec_path` 不解析符号链接，所以经 `/usr/bin/java`（符号链接）启动时 `$ORIGIN` 与 libjli 推导的 `java.home`
  都落在 `/usr/bin`。**正解**是在 `kernel/proc/exec.c` 记录 `exec_path` 时做 realpath（`vfs_resolve`）；当前用 overlay 的
  `/usr/bin/java` 薄封装绕过：直接 exec 真实路径。
- 另有堆参数问题：不显式给 `-Xms/-Xmx` 时 VM 报 `Too small maximum/initial heap`（guest 内 `MemTotal` 1048576 kB、
  `MemAvailable` 541068 kB）。封装默认 `-Xms256m -Xmx512m`，命令行参数仍可覆盖。
- 提示：`MemTotal` 只有 1 GB（该实例 QEMU 给的是 2G）也值得单独核对 `pfa.total_frames` 的口径。

### 桌面壁纸不显示（backdrop 不渲染）
- 症状：xfdesktop 在跑（桌面图标正常、root 被背景层覆盖），但桌面是纯黑；1024x768 下 94% 像素为 `(0,0,0)`。
- 已核实：`/backdrop/screen0/monitor<id>/workspace0/last-image` 的 `<id>` 用的是 `sha1(connector)`，而 `sha1("Virtual-1")` **正好在** overlay 的 9 个 monitor key 里，所以不是 key 不匹配；`image-style=5`、`color-style=0` 合法；改成 SVG 壁纸并显式加 `image-show=true` 后**仍然全黑**。
- 新发现：镜像里 **没有 jpeg/png 的 gdk-pixbuf loader**（`loaders.cache` 只有 ani/bmp/gif/icns/ico/pnm/qtif/svg/tga/tiff/xbm/xpm），`gdk-pixbuf` 也不依赖 libjpeg/libpng，所以 `xfce-blue.jpg` 本来就无法解码（这也是已改指向 `xfce-flower.svg` 的原因）。但换成 SVG 后壁纸依旧不渲染，说明还有第二个原因。
- 提示：需要在 guest 内 dump `xfconf-query -c xfce4-desktop -lv` 看 xfdesktop 实际读到的 backdrop 树，以及 `gdk-pixbuf-query-loaders`。本轮无法做这个诊断：guest 写盘后 ext4 位图校验和就不一致（见下条），无法从宿主机读回 guest 写过的文件。

### guest 写盘后 ext4 位图校验和不一致
- 症状：`image-world` 产物 `e2fsck -fn` 干净；但 guest 启动一次后，从宿主机看（QEMU `-snapshot` 的 qcow2 overlay，或 kill 后的镜像）会出现 `Block bitmap checksum does not match bitmap`，`debugfs` 直接打不开。
- 意义：这解释了本文档「测试环境注意事项」里那条「损坏来源未定位」——**损坏不是构建期引入的，是 guest 写入时产生的**。损坏的 rootfs 会伪造内核 bug（udev worker 超时、应用起不来），排查前务必先 `e2fsck -fn`。
- 提示：怀疑 A20OS 的 ext4 写入路径没有同步 `metadata_csum` 的位图校验和（`^has_journal` 下没有日志回放来兜底）。需要在内核 ext4 写路径上核对 group descriptor/bitmap 的 checksum 更新。

### 面板下拉菜单项不启动应用
- 症状：面板按钮能开菜单，但点下拉菜单项既不启动应用（无新 `xdg_surface`、无窗口），也不关菜单。
- 现状（在 `e2fsck` 干净的 rootfs 上复验）：指针移动与坐标正常——悬停分类项会弹出子菜单；点击面板按钮会打印 `press on layer-(sub)surface`。
- 注意：这与上面已解决的「窗口控制键无效」是**两个不同症状**——窗口控制/聚焦/拖动已由 labwc 默认绑定修复，本条的客户端菜单条目点击仍未定位。
- 已排除：内核输入通路本身。`struct input_event` ABI、PS/2 按键解码、每包 `SYN_REPORT`、evdev 打戳、内核与 vDSO 的 CLOCK_MONOTONIC 时基逐项核对一致；libinput 也枚举到了设备（`Adding A20OS evdev mux [0:0]`）。
- 提示：是否打日志取决于命中节点的类型（只有层表面分支会打印），所以「落在子菜单上的按下不打印任何 labwc 日志」本身是正常的，不能当作事件没送达的证据。下一步应在 guest 内直接看 `wlr_scene_node_at` 的命中结果与客户端实际收到的 `wl_pointer.button`，不要再从截图推断。labwc 会把每个执行到的 action 记进日志（`[action.c] Handling action <n>: <Name>`），这是比截图可靠的观测手段。

### dbus 同步调用超时 / 对端 pid 为 -1
- 症状：会话里 `NoReply`，xfsettingsd/panel 报 Xfconf/dconf 超时后降级；dbus 日志里身份异常：`dbus-daemon[44]: [session uid=0 pid=18446744073709551615 pidfd=5] ...`（pid 为 -1）。
- 现状：在 `e2fsck` 干净的 rootfs 上稳定复现。同一行里 `requested by ':1.7' (uid=0 pid=92 comm=".../xfsettingsd")` 的 pid 却是对的。
- 已修（SO_PEERCRED 部分）：`kernel/net/socket_unix.c` 的 connect 路径把**发起端自己**的 pid 写进了 `s->peer_pid`，而发起端的对端应是服务端。现在 bind 时记录属主凭据（`net_socket_t.owner_pid/uid/gid`），connect 时用它给发起端填 `SO_PEERCRED`；accept 出的子 socket 那一路原本就对。
- 仍未修：改完后 dbus 日志里的 `pid=18446744073709551615` 依旧。同一行 `pidfd=5` 说明 `SO_PEERPIDFD` 已经拿到 pidfd，所以 dbus 显示的这个 `pid=` **不是**取自 `SO_PEERCRED`，而是 `SO_PASSCRED` 的 `SCM_CREDENTIALS`（`ch_cred_*` 路径）。下一步查 AF_UNIX 首包（dbus 的初始 NUL 字节）的 SCM_CREDENTIALS 为什么是 -1。
- 已排除：`peer_pid` 未初始化（`net_socket_alloc()` 用 `obj_cache_alloc_zero()`）；`getpid()` 本身（`dbus-daemon[44]` 说明任务 pid 正确）。

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
