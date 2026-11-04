# 桌面与 Minecraft 当前边界

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

来源：原 `docs/distro/minecraft.md` 的「当前边界」一节，以及后续实现记录的结论。迁移时删除了日期、镜像哈希与逐次运行日志，只保留仍然成立的边界。

- 854×480、4 区块视距实测可以移动和操作背包；观察到约 8–18 FPS，菜单约 20 FPS。这是功能验证中的现场读数，仍需性能优化，未达到稳定 30/60 FPS。
- OpenAL 使用 musl 系统库，sound engine 能启动；当前配置的设备是 No Output，尚未验收有声播放。原始 glibc C++ OpenAL 的异常路径不再进入 JVM。内核 PCM 已通过 `smoke-audio-userspace` 门禁（440 Hz 注入 QEMU 的 wav 后端），但桌面里的 ALSA 客户端还打不开它：`/proc/asound/cards` 不存在，alsa-lib 无法解析 `hw:0`；见下方“音频”。
- LWJGL C native 仍经 gcompat；启动器选择系统 allocator。没有宣称完整 glibc ABI 兼容，也没有为 jemalloc 注入补丁。
- Wayland 不支持绝对设置鼠标位置的 GLFW 提示仍可见；键盘移动与背包已经实测，鼠标转向和跳跃也已实测；全屏及真实硬件输入尚未系统验收。QEMU 的`virtio-tablet` 会被 libinput 以“missing tablet capabilities: resolution”忽略，所以绝对指针设备在本平台上尚未验收。
- 默认 token 为 0，服务端会返回真实的 401；不把它误报为 DNS/TLS 故障。当前没有集成 Microsoft 账号登录，不宣称 Realms 或多人服务器可用。
- GEM 句柄命名空间仍是设备级，尚未实现 per-open 隔离；fence 当前同步完成，不是异步 GPU scheduler；DMA-BUF CPU 同步目前覆盖线性 level-0 纹理。

[组件状态](../../distro/minecraft-components.md) 记录 JVM、native 和音频的具体范围。

## 桌面会话

`instances/xfce-*.toml` 启动的是 labwc + XFCE 组件的 Wayland 会话，不是 xfce4-session 的 X11 会话：

- `start-xfce4-session` 建立 dbus 会话总线，`session-start` 先发布`org.xfce.SessionManager`，再把会话交给 labwc。面板的 actions 插件通过这个名字查询 `CanShutdown`/`CanRestart` 并执行动作；没有它时插件会退回到`shutdown --poweroff`，而镜像里没有该命令，电源菜单因此什么也不做。
- `session-power.py` 提供该接口：`Shutdown`/`Restart` 走内核 `reboot(2)` 路径（先 `vfs_system_shutdown` 刷盘/卸载，再关机或重启），`Log Out` 结束合成器。
- `start-desktop-components.sh` 把 actions 插件的菜单项发布为`Log Out / Restart / Shut Down`：默认列表隐藏 Restart，并列出本会话无法执行的 Suspend、Switch User、Lock Screen。
- `/sbin/init` 在会话退出后重启它（最多 3 次，间隔 2 s）；第 3 次仍退出则`poweroff`，避免留下没有用户态系统的 kiosk。
- 壁纸由 `swaybg` 在 xfdesktop 的桌面表面之后创建 background 层表面； xfdesktop 自己仍不绘制 backdrop，但桌面图标和面板不受影响。
- 面板下拉菜单项（如 Terminal Emulator）能启动应用并关闭菜单；终端里的 Backspace 正常删除字符。
- 冷启动日志里不再出现 `Failed to set CRTC`；帧缓冲对象持有 GEM 引用， dumb buffer 的销毁不再把正在 scanout 的 handle 释放。
- x86_64（KVM，4 vCPU）和 riscv64（TCG，4 vCPU）都完成过同一组交互：面板 Applications 菜单启动 Terminal、终端里输入命令与 Backspace、启动 Thunar 浏览 `/root`，最后从电源菜单正常关机（`[SD] POWER_OFF entry`，x86_64 与 riscv64 各一次）。riscv64 上第一次关机曾因为文件操作在途被`mntns_shutdown_begin()` 以 `-EBUSY` 拒绝，`session-power.py` 现在按内核的 RETRY_* 契约做有界重试。普通 `poweroff` 没有这层重试：Minecraft 还在播放音乐时，第一次 `vfs_system_shutdown` 也会返回 `-EBUSY`，shell 的`poweroff -f` 因此失败（会话电源菜单会重试并成功）。
- aarch64（TCG，4 vCPU、`smp = 4`）冷启动到完整桌面，`[SMP] 4/4 configured CPUs online`，运行 10 分钟以上无 panic；该轮 eudev 的 input worker 超时被杀， libinput 没有接管键盘，所以键盘/按钮输入未能在 AArch64 上验收（相对指针移动仍可见）。同一镜像在 x86_64 与 riscv64 上输入正常。
- AArch64 SMP4 的共享状态负载：`sched_stress`、`proc_stress`、`futex_stress`、`mm_stress`、`scm_stress` 在 `NR_CPUS=4` 构建以 `-smp 4` 全部 PASS，约 30 分钟无 KERNEL TRAP/PANIC，调度器违规为 0。

## 输入、全屏与指针边界

- 输入链路：QEMU 的虚拟键盘/鼠标事件进入内核 `input_mux` 的 evdev 流，再由 udev/libinput 交给客户端。实测在终端里输入命令与 Backspace、用菜单启动应用、用鼠标点击面板；串口日志里的 `[INPUT]` 行证明事件走的是内核设备，不是应用层注入。物理板卡不在这些 QEMU 实例的承诺平台内。
- 全屏：`xfce4-terminal --fullscreen` 覆盖整个输出（含面板）；Minecraft 在 XWayland 下按 F11 后窗口填满 1024×768。两者都有截图。
- 绝对指针：Wayland 不提供“把指针移到指定位置”的协议，GLFW 会打印`Wayland does not support setting mouse position`。这是协议边界，不是 A20OS 缺功能；Minecraft 的鼠标转向用相对运动工作。QEMU 的`virtio-tablet` 因缺少 ABS resolution 被 libinput 忽略，不能作为绝对指针验收设备。

## 音频

- 内核把 HDA/virtio-snd 暴露成 ALSA PCM：`/dev/snd/controlC0`、`pcmC0D0p`、`pcmC0D0c`。原生 `audioplay` 与 `smoke-audio-userspace` 门禁都在这条路径上验证过。
- 桌面 ALSA 客户端可以打开设备并播放。修复的是 ALSA 兼容层的 UAPI 细节：`SNDRV_CTL_IOCTL_PVERSION` 返回 2.0.9（不是 1.1.6），CARD_INFO 使用 Linux 的 376 字节布局与 `0x81785501`，PCM_NEXT_DEVICE/PCM_INFO 使用 Linux 编号，接受 `PREFER_SUBDEVICE(-1)`、`USER_PVERSION`、`SYNC_PTR`、`STATUS_EXT`、`DELAY`，`HW_REFINE` 把应用约束与硬件能力求交集（不再覆盖），`HW_PARAMS`把每个参数收敛成单一值并返回一致的 frame/period/buffer 关系。
- `alsoft.conf` 现在选择 `drivers = alsa`（不再是 null/No Output）。实测：OpenAL 打开设备、source 进入 PLAYING 并结束；mpv `--ao=alsa` 播放 440 Hz 测试音； QEMU 的 wav 后端录到 441 Hz、非静音（峰值等于注入幅度，RMS 约 5.5k–8k）。
- Minecraft 的 LWJGL OpenAL 也在 `ALSA Default` 上初始化并启动 sound engine；标题画面音乐被 QEMU 的 wav 后端录下（33 秒、非静音、主频约 185 Hz）。语音聊天使用的 16 kHz 采集设备仍未开放（驱动只报告 48 kHz），游戏记录`failed to set sample rate near 16000` 后跳过它。

## JVM 与调试信息

- `/usr/bin/java` 是固定 `JAVA_HOME` 的封装；经符号链接启动 JVM 也能工作，因为 exec 从打开文件记录最终路径，`$ORIGIN` 解析到真实 `bin` 目录。实测 `/root/java-link -version` 与经该链接运行的 Java 程序都正常。
- `/proc/<pid>/fdinfo/<fd>` 对 pidfd 输出 `Pid:`/`NSpid:`。dbus 用它解析自己的凭据；没有这两行时日志里的 `pid=` 是 `18446744073709551615`。SCM_CREDENTIALS 与 SO_PEERCRED 的 pid 由 `scm_stress` 的 listen/accept 用例覆盖。

## 文件系统写回与关机

- full profile 默认使用 ext4 的 Rust extent 事务（`EXT4_EXTENT_TXN_IMPL=rust`）：桌面写出的文件会形成 depth-1 extent 树，C 的 root-trim 路径拒绝这种形状，导致 unlink 后的 orphan 无法回收。Rust 事务展开 depth-0/1 树并重建单一根，因此删除大文件时不再留下 orphan。
- `ext4_shutdown_clean()` 在提交 CLEAN 之前重放并回收残留 orphan：被删除时仍有 vnode 引用的 inode 会留在孤儿链上，而 vnode 缓存可以活过会话；这一步保证正常关机后的镜像没有待恢复条目。
- 实测：x86_64 桌面上两轮写负载（200 个小文件、一个 6 段稀疏文件、缩略图与 xfconf 写入）→ `poweroff` → 宿主 `e2fsck -fn` 两轮都干净；同一内核上`vfs_stress`、`mm_stress`、`mtcorrupt_test`、`scm_stress` 全部通过。

## 进程与内存稳定性

- 匿名 fault-around 窗口在 VMA 被并发合并后把良性身份变化判成失败，是 mpv/LuaJIT、tumblerd、JVM 那条“指针字段变 NULL”的根因；现在窗口重试并按当前 VMA 夹紧，`smoke-mtcorrupt` 在 `NR_CPUS=4` 上守住它。
- 三处“自己翻译物理帧再直接写”的路径（clear_child_tid、PI futex、信号栈 exec 重映射）改为先做受检查的用户写/COW 破坏。
- 实测：20 次 `mpv --vo=null --load-scripts=yes` 无崩溃；`java -version` 20/20 退出码 0；tumblerd 处理缩略图请求后正常退出；40 次 `mpv --vo=null` 的进程创建/销毁 churn 后桌面组件仍在。
- mpv 的 Lua 脚本工作已解除：`mpv.conf` 不再设置 `load-scripts=no`。

## 实现范围

内核侧为 Minecraft 提供的修复都经通用接口提供，没有按进程名或 ISA 名特判：

- CPU 信息报告实际在线 CPU 数，架构身份由 `kernel/arch` hook 提供。
- 普通路径与 openat2 共用挂载感知的符号链接遍历；GPU sysfs 暴露真实 PCI/MMIO 身份，动态属性走文件 read，Mesa 可正确发现 GPU。
- `VIRTGPU_RESOURCE_CREATE` 分配独立 GEM 对象；PRIME/DMA-BUF 导出保留真实 VMO，mmap 与`SCM_RIGHTS` 共享同一对象。
- 3D 上传/下载使用正确 wire 布局，scanout 使用宿主 3D 资源，提交等待真实 host GPU fence。
- 大命令与 backing attachment 使用完成中断唤醒，保留启动阶段的有限轮询。
- `SIOCGIFCONF` 的 NULL 尺寸查询与输出缓冲区语义已修正，Java 能正常枚举网卡。

详见 [GPU 共享与同步](../../graphics/virtgpu-sharing-and-sync.md)、[GPU 发现](../../graphics/drm-sysfs-discovery.md) 与[网卡枚举](../../kernel/linux-abi/interface-enumeration.md)。

## 已采用的规避

- **视频默认处理器是 `ffplay`**：这是当前行为，不是临时手段；mpv 现在可以正常运行，但默认 MIME 处理器仍是 ffplay。
- **JVM 走 `/usr/bin/java` 封装**：JDK 需要堆参数，封装脚本补上；`$ORIGIN`已由 exec 的最终路径保证，不再依赖包装脚本的路径。
