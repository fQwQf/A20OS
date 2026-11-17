# 桌面与 Minecraft 当前边界

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

来源：原 `docs/zh/distro/minecraft.md` 的「当前边界」一节，以及后续实现记录的结论。迁移时删除了日期、镜像哈希与逐次运行日志，只保留仍然成立的边界。

- 854×480、4 区块视距实测可以移动和操作背包；观察到约 8–18 FPS，菜单约 20 FPS。这是功能验证中的现场读数，仍需性能优化，未达到稳定 30/60 FPS。
- OpenAL 使用 musl 系统库和 ALSA Default；本轮 x86_64 游戏两次均生成非静音 QEMU WAV，播放路径已通过，见下方“音频”和[应用证据](../../kernel/linux-abi/application-contracts.md#x86_64-桌面与-jvm-的直接运行2026-11-16)。原始 glibc C++ OpenAL 的异常路径不再进入 JVM；不把 QEMU 录音计为实机音频或采集认证。
- LWJGL C native 仍经 gcompat；启动器选择系统 allocator。没有宣称完整 glibc ABI 兼容，也没有为 jemalloc 注入补丁。
- Wayland 不支持绝对设置鼠标位置的 GLFW 提示仍可见；键盘移动与背包已经实测，鼠标转向和跳跃也已实测；全屏及真实硬件输入尚未系统验收。QEMU 的`virtio-tablet` 会被 libinput 以“missing tablet capabilities: resolution”忽略，所以绝对指针设备在本平台上尚未验收。
- 默认 token 为 0，服务端会返回真实的 401；不把它误报为 DNS/TLS 故障。当前没有集成 Microsoft 账号登录，不宣称 Realms 或多人服务器可用。
- GEM 句柄命名空间仍是设备级，尚未实现 per-open 隔离；fence 当前同步完成，不是异步 GPU scheduler；DMA-BUF CPU 同步目前覆盖线性 level-0 纹理。

[组件状态](../../distro/minecraft-components.md) 记录 JVM、native 和音频的具体范围。

## 桌面会话

`instances/xfce-*.toml` 启动的是 labwc + XFCE 组件的 Wayland 会话，不是 xfce4-session 的 X11 会话：

- `start-xfce4-session` 建立 dbus 会话总线，`session-start` 先发布`org.xfce.SessionManager`，再把会话交给 labwc。面板的 actions 插件通过这个名字查询 `CanShutdown`/`CanRestart` 并执行动作；没有它时插件会退回到`shutdown --poweroff`，而镜像里没有该命令，电源菜单因此什么也不做。
- `session-power.py` 提供该接口：`Shutdown`/`Restart` 请求原版 OpenRC 执行正常服务停止、最终 TERM/KILL 与 checked 电源调用；`Log Out` 请求 OpenRC 重启桌面服务。
- `start-desktop-components.sh` 把 actions 插件的菜单项发布为`Log Out / Restart / Shut Down`：默认列表隐藏 Restart，并列出本会话无法执行的 Suspend、Switch User、Lock Screen。
- OpenRC 的 `a20-desktop` 服务由 `supervise-daemon` 监督；当前配置为 2 s 重启延迟、30 s 窗口内最多 5 次，错误保留为服务失败状态，具体配置见 `packages/overlay/xfce/etc/init.d/a20-desktop`。
- 壁纸由 `swaybg` 在 xfdesktop 的桌面表面之后创建 background 层表面； xfdesktop 自己仍不绘制 backdrop，但桌面图标和面板不受影响。
- 面板下拉菜单项（如 Terminal Emulator）能启动应用并关闭菜单；终端里的 Backspace 正常删除字符。
- 冷启动日志里不再出现 `Failed to set CRTC`；帧缓冲对象持有 GEM 引用， dumb buffer 的销毁不再把正在 scanout 的 handle 释放。
- x86_64（KVM，4 vCPU）和 riscv64（TCG，4 vCPU）都完成过同一组交互：面板 Applications 菜单启动 Terminal、终端里输入命令与 Backspace、启动 Thunar 浏览 `/root`，最后从电源菜单正常关机（`[SD] POWER_OFF entry`，x86_64 与 riscv64 各一次）。riscv64 上第一次关机曾因为文件操作在途被`mntns_shutdown_begin()` 以 `-EBUSY` 拒绝，`session-power.py` 现在按内核的 RETRY_* 契约做有界重试。普通 `poweroff` 没有这层重试：Minecraft 还在播放音乐时，第一次 `vfs_system_shutdown` 也会返回 `-EBUSY`，shell 的`poweroff -f` 因此失败（会话电源菜单会重试并成功）。
- aarch64（TCG，4 vCPU、`smp = 4`）冷启动到完整桌面，`[SMP] 4/4 configured CPUs online`，运行 10 分钟以上无 panic；该轮 eudev 的 input worker 超时被杀， libinput 没有接管键盘，所以键盘/按钮输入未能在 AArch64 上验收（相对指针移动仍可见）。同一镜像在 x86_64 与 riscv64 上输入正常。
- AArch64 SMP4 的共享状态负载：`sched_stress`、`proc_stress`、`futex_stress`、`mm_stress`、`scm_stress` 在 `NR_CPUS=4` 构建以 `-smp 4` 全部 PASS，约 30 分钟无 KERNEL TRAP/PANIC，调度器违规为 0。

## AArch64 原生服务恢复（2026-11-16）

PL031 用户态驱动现使用该板的实际 MMIO 页 `0x09010000` 与 GIC INTID 34；板级授权仅允许固件枚举到的 RTC 页，C 与 Rust 服务共用协议。PL031 只能读整秒，短闹钟保守向上取整，本轮 100 ms 请求约 2 s 后经真实 IRQ 回答。

监督程序及独立恢复测试改用 Native 线程信号掩码屏蔽 SIGCHLD，以 EXITED 事件跟踪子服务；测试结束发送 SIGKILL 并等待退出，替换原来仅查询存活状态的 signal 0。另修复 Native 目录创建、挂载、卸载及服务注册未应用 chroot 根目录的问题，目录与 mount tree 现在使用共同的 dirfd/chroot 解析器。

公开门禁为 `tools/probes/aarch64_supervision_probe.py` 与 `tools/probes/aarch64_desktop_supervision_probe.py`。前者在 SMP4 开发镜像分别通过 C、Rust 监督程序/RTC；后者在真实 XFCE/OpenRC world、labwc 与 xfce4-panel 运行期间核对三轮 HANG、健康超时、SIGKILL、重新绑定、旧端点失效及越权终止拒绝，同时完成 `/ufs` 文件写回/读回和卸载。关机前先卸载用户态文件系统，保持服务器活着完成协议；OpenRC 的 final KILL 若先杀服务器，内核会拒绝不安全关机，这一失败日志保留。

证据位于 `build/evidence/v1-closeout-20261116/`：开发镜像 `aarch64-supervision-{c,rust}-verified/report.json`，桌面 C/Rust 公开门禁分别为 `aarch64-desktop-supervision-c-public/report.json` 与 `aarch64-desktop-supervision-rust/report.json`。两者内核 SHA-256 为 `2fdb76c9b6be8ab8eb88b7948e4340480d4342e0e844c782b5d27d697ae238d2`，QEMU 自然退出 0、关机审计 0，实际写入的桌面盘和服务盘分别通过 e2fsck / fsck.fat。测试使用私有磁盘副本，未改写输入镜像。此项关闭 DESKTOP-14，不扩大到 AArch64 键盘/按钮输入或完整桌面交互认证。

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

## 2026-11-16 本轮 x86_64 客户端操作与持久化

真实 XFCE/OpenRC world、q35/KVM SMP4、4 GiB、854×480 Wayland/virgl 中，完成移动、
跳跃、背包、相对鼠标转向、正常保存/退出及同盘冷启动实际重载。存档初始位置为
`[-43.5, 67.0, 88.28841888309817]`，冷启动服务器在完全相同位置登录；之后羊碰撞使
即时 F3 坐标略有变化。指针进入客户端后 pointer lock 与三次原始相对事件正确改变
朝向，最终 NBT 为约 `[6, -1.5]`。两次 OpenAL ALSA Default、非静音 WAV、自然关机及
ext4/FAT 检查通过。运行报告与观察记录统一见
[本轮应用契约](../../kernel/linux-abi/application-contracts.md#x86_64-桌面与-jvm-的直接运行2026-11-16)。

这些已完成行为不再作为待验收；首次观察进程/客体提前终止的尝试仍保留为失败。
本轮没有扩大账号/联机、物理输入/采集或其它架构的认证，也不把浏览器的独立失败计为通过。


## 关机期间 xfsettingsd 崩溃（2026-11-17）

Console 定向 guest 在 q35/KVM、ABI=both、SMP4 中完成各 64 字节双向收发、重开/drain。
随后通过 SessionManager 请求正常关机，OpenRC 停止 a20-desktop 后，`xfsettingsd`
（pid 448，Linux ABI）触发 SIGSEGV：`pc=0x9b02c2bd`，地址 `0xb1667500`，指令字
`0x48008b48`。该轮自然 S5、QEMU/ext4/FAT 均为 0，不能抵消已观察到的进程崩溃；
该历史报告保持失败；后续已定位并修复 DESKTOP-15，见下方关闭记录。

证据 `build/evidence/v1-closeout-20261116/virtio-console-stop/guest-fixed/`，保留完整
日志、输入哈希与未改写的失败报告。当时尚待定位并复验；后续已完成一次针对性关机验证，见下方记录；
不重复浏览器、游戏和已关闭的服务恢复矩阵。


### DESKTOP-15 定位进展

原轮的持久内核日志补足了串口限流的寄存器和 VMA：故障是 x86 #GP，共用 cause=1；
`stval=0xb1667500` 是旧 CR2，不能用作实际访问地址。`rax=0xaaaaaaaaaaaaaaaa`，
`rdi=0x9cbf4c80`；实际故障指令 `mov (%rax),%rax` 位于 `libgobject-2.0.so.0.8600.5`
的 `g_type_check_instance_is_fundamentally_a+0xd`（ELF 偏移 `0x3a2bd`）。提取原镜像 ELF，
代码页头 16 字节、故障指令及持久日志 VMA 一致，定位到失效对象类型指针的读取。

只做两次定向诊断：透传 `g_object_unref` 记录调用者，不跳过释放、不改生产内核/
可执行文件。普通关机未记录主函数清理；显示服务仍活着时直接 SIGTERM 则执行了
清理，未复现同类错误、无 core。两轮分别 28.236 / 28.410 秒自然关机，磁盘检查为 0。
这两次未复现不等于修复；当时具体失效回调未确定，DESKTOP-15 保留。记录和冻结诊断
源码/ELF/日志见 `build/evidence/v1-closeout-20261116/desktop-shutdown/analysis.json`。


### x86_64 core 寄存器格式修复（2026-11-17）

定位 DESKTOP-15 时确认旧 core 路径把 RAX 覆盖为 PC，并对 x86_64 写出 RISC-V 的
32 槽寄存器/FP 描述符；PRPSINFO 的 flag 宽度也不符。现保留完整通用寄存器，按
ELF machine 序列化 x86_64 的 27 槽、336 字节 PRSTATUS 和 136 字节 PRPSINFO，
arch hook 提供保存的 CS/SS；TLS base 的来源随后按下节修正。未取得的 FP 不再伪装成有效镜像；其余
端口仍保留既有兼容边界，未扩大为全架构 core/FPU/NT_FILE 项目。

15 个既有 Rust 用例，以及链接生产 x86 trap 导出、C adapter 与 Rust 序列化器的
Linux 头文件消费者通过；GDB 正确读取其生成的合成 core 中 RAX/RIP/RSP，整个宿主
脚本 0.706 秒。标准 x86_64 SMP4 C/Rust 两对象编译通过；无整内核或新增关机重放。
合成 core 的任务名用于测试，不是原 xfsettingsd 的崩溃文件。证据
`build/evidence/v1-closeout-20261116/x86-core-notes/report.json` 冻结源码、系统头文件、
构建对象、样本与 GDB 输出。该修复只解决诊断格式缺陷，当时原失效回调未确定，
未据此关闭 DESKTOP-15。


### DESKTOP-15 真实 core 与会话监督停止（2026-11-17）

日志监督器原来没有 TERM 处理，OpenRC 停止时只结束监督器，不能转发停止、排空输出
或记录子会话退出。现新建子会话组，处理 TERM/INT/HUP，在普通循环转发并等待/回收；
3 秒宽限后仍运行才 KILL，状态不改写为成功，保留 OpenRC TERM/5 的回收/刷盘余量。
输出先 EOF 也继续监督活着的 owner，异常路径清理直接 owner 并恢复信号处理器。
7 个定向宿主用例通过，旧代码负控因监督器直接被 TERM 结束而失败。

一次故障专用探针的正常关机未复现（29.119 秒），不关闭原缺口。随后移除 preload、
验证生产监督器时，停止请求和子会话退出记录各 1 次，间隔 3.062137 秒，无强制 KILL；
但 xfsettingsd 再次崩溃，整轮仍判失败。32.270 秒自然关机、QEMU 与磁盘检查为 0
不覆盖该失败。本次取得 8 MiB 真实 core；GDB 与映射 ELF 确认 main+0x4f7（返回偏移
0x9527）在释放 GTK Settings helper，经 g_object_unref 访问已填充 0xaa 的对象。
D-Bus 断开警告与[上游 helper 源码](https://raw.githubusercontent.com/xfce-mirror/xfce4-settings/xfce4-settings-4.20.1/xfsettingsd/gtk-settings.c)
对应：bus_acquired 已释放 callback 引用，后续 name_lost(NULL) 再释放，提前消耗 main
仍持有的引用。当时仍需修正该回调生命周期并验证正常退出；监督器修复不代替此项。
证据 `build/evidence/v1-closeout-20261116/desktop-session-stop/report.json`（整体失败），
保存生产代码、旧代码负控、日志、core、符号化栈及上游源码。当时 DESKTOP-15 继续开放，随后按下节修复关闭。


### core TLS 来源修正（2026-11-17）

真实 core 显示上一轮 hook 读取的 IRQ frame TLS 槽不是保存值（FS=0、GS=0x246）。
trap.S 明确不保存 FS，并复用中断 frame 的部分槽；现改为当前 fatal-signal 任务的
FS_BASE 与 SWAPGS 停放的用户 GS MSR，沿用已有 arch_read_user_* 助手，CS/SS 仍取
保存的 CPU frame。共享 ptrace 导出不改。宿主 C/Rust+Linux 头文件/GDB 检查将 MSR
替代值刻意设为不同于 frame 槽，15 个原 Rust 用例和消费者通过，标准 C adapter
对象编译通过；本次不追加 guest，不能宣称实读 MSR 验收。旧真实 core 保持原始字节，
其 GPR/栈定位 GTK helper 调用的证据不受影响。
`build/evidence/v1-closeout-20261116/x86-core-tls/report.json` 记录这次来源修正和边界。


### DESKTOP-15 修复与关闭（2026-11-17）

GTK Settings helper 改用独立弱引用作为 D-Bus 回调数据，访问对象的回调临时取得并
释放自己的强引用；`name_lost(NULL)` 不再消耗 main 的引用，helper 结束时撤销
name owner，回调数据由 GIO 回收。GSettings/xfconf 信号连接跟随 helper 生命周期。
补丁保留正常释放与 X11/Wayland 功能，没有 preload 或跳过清理。

`mkrootfs.py` 自动从已校验的官方 4.20.1 源码构建并安装修复，拒绝未经复核的包版本；
镜像随带对应源码、补丁、构建脚本和许可证。安装器的真实暂存根目录验证通过：错误
版本不覆盖文件，正确版本安装的 ELF 哈希与实际构建一致。构建及升级边界见
[补丁说明](../../../../packages/patches/xfce4-settings/README.md)。

生产回调/注册/析构代码在真实隔离 GIO D-Bus 上完成回归：原版断开后提前销毁 helper，
补丁版保留 main 引用，并安全处理回调尚未分发时的销毁。一次 x86_64 KVM q35 SMP4
XFCE/OpenRC 正常关机验证运行了真实新 ELF，核对哈希和 GTK Settings 导出。持久日志
仍记录同一 D-Bus 断开警告，未再出现 FATAL/core；停止与子会话退出记录各 1 次，间隔
3.047974 秒，无监督器强制超时。32.193 秒自然关机，QEMU/ext4/FAT 检查均为 0。
会话 owner 的 `-15` 是正常停止时保留的 TERM 状态，没有改写为成功。

证据 `build/evidence/v1-closeout-20261116/xfsettingsd-lifetime/report.json` 冻结补丁、
构建与安装器结果、生产消费者回归、日志和输入哈希。原始失败 core/报告继续保留。
镜像夹具首次漏建源码目录的错误日志保留；测试 ELF 安装正确，关机后离线补齐源码
目录并核对每个文件，不追加 guest 重放。复用的内核只用于隔离应用修复，并非最新
全系统候选；没有扩大其它平台、core TLS 实读、浏览器/游戏或 N18/N20 的认证。
DESKTOP-15 达到原定关闭条件，从缺口清单删除。
