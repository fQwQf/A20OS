# DESKTOP 缺口

对应 V1-DESKTOP。现状见 [current/desktop-minecraft-status.md](../current/desktop-minecraft-status.md)；
仍未关闭问题的诊断见 [notes/](notes/README.md)。

### DESKTOP-01 AArch64 SMP4 竞态

- 现状：观察到两种不同的 SMP 竞态（IPI 风暴后 trap frame 损坏；SIGSEGV 投递期跨任务寄存器交错），当前以 `smp=1` 运行。
- 缺口：竞态未定位，SMP1 不是修复；AArch64 桌面没有多核验收。
- 关闭条件：SMP4 下长时运行无 KERNEL TRAP/PANIC，且用例实际命中共享状态；SMP1 降级不再需要。
- 来源：原 `known-issues.md`、[instances/xfce-aarch64.toml](../../../instances/xfce-aarch64.toml)。

### DESKTOP-02 RISC-V 桌面交互

- 现状：RISC-V 桌面可启动；MCS 持锁调度、`pgrep` 崩溃与交互复验缺失。
- 缺口：x86_64 的交互验收不覆盖 RISC-V。
- 关闭条件：RISC-V 上完成真实输入/窗口/终端/文件管理器负载并自然关机。
- 来源：原 `known-issues.md` 当前工程队列。

### DESKTOP-03 应用退出异常

- 现状：COW、FP、TLS 的既有修复不能证明症状消失。
- 缺口：mpv/LuaJIT、tumblerd 与 JVM 的退出异常仍需在干净镜像上复现并定位。
- 关闭条件：三者各自能稳定启动、退出并回收资源；或给出明确的既有缺陷归属。
- 诊断：见 [notes/desktop-jvm-mpv-exit.md](notes/desktop-jvm-mpv-exit.md)。
- 来源：原 `known-issues.md` 当前工程队列。

### DESKTOP-04 JVM `$ORIGIN` 端到端

- 现状：exec 已从打开文件记录最终路径，`exec-symlink-origin` 通过。
- 缺口：JVM 经符号链接启动的端到端验收缺失。
- 关闭条件：JVM 经符号链接启动并正常工作，路径解析无回归。
- 来源：原 `known-issues.md` 当前工程队列。

### DESKTOP-05 输入与显示范围

- 现状：键盘移动与背包已实测；鼠标转向、跳跃有实测记录。
- 缺口：全屏、真实硬件输入、绝对鼠标位置提示未系统验收；音频当前是 No Output。
- 关闭条件：每项在承诺平台上给出真实输入与输出证据；音频有声播放可验收。
- 来源：[current/desktop-minecraft-status.md](../current/desktop-minecraft-status.md)。

### DESKTOP-06 Minecraft 性能

- 现状：854×480、4 区块视距下约 8–18 FPS，菜单约 20 FPS。
- 缺口：未达到稳定 30/60 FPS，也没有性能目标与对照。
- 关闭条件：给出目标帧率与同输入对照，达到目标或把目标改为有依据的契约。
- 来源：[current/desktop-minecraft-status.md](../current/desktop-minecraft-status.md)。

### DESKTOP-07 桌面扩展写盘一致性

- 现状：journal 窄门禁不覆盖所有桌面写负载。
- 缺口：guest 写盘后的 ext4 位图一致性仍需按历史触发场景复验。
- 关闭条件：桌面写负载后关机再 `e2fsck` 干净；与 FS-01/FS-02 联动。
- 来源：原 `known-issues.md` 当前工程队列。

### DESKTOP-08 x86_64 桌面组件随机 SIGSEGV

- 现状：xfdesktop/panel/thunar/gst/udevd/dbus-daemon 随机野指针崩溃，只在 x86_64 出现；fix8–fix11 已把崩溃点定位到 musl 自己的 `hlt` 断言，且崩溃后 panel 主进程仍存活。
- 缺口：根因未定位；崩溃间歇，无法稳定复现。
- 关闭条件：长时桌面运行不再出现 SIGSEGV；或定位到具体缺陷并修复，负控能复现旧行为。
- 诊断：[notes/desktop-x86-sigsegv.md](notes/desktop-x86-sigsegv.md)。

### DESKTOP-09 桌面壁纸不渲染

- 现状：xfdesktop 在跑、桌面图标正常，但背景纯黑（1024×768 下 94% 像素为 `(0,0,0)`）。
- 缺口：backdrop 渲染路径未通。
- 关闭条件：冷启动后壁纸实际显示（像素检查），且不靠循环重启掩盖。
- 诊断：[notes/desktop-wallpaper.md](notes/desktop-wallpaper.md)。

### DESKTOP-10 面板下拉菜单项不启动应用

- 现状：面板按钮能开菜单，点下拉条目既不启动应用也不关菜单；内核输入通路已排除。
- 缺口：命中节点与客户端 `wl_pointer.button` 的链路未定位。
- 关闭条件：点击菜单项启动应用并关闭菜单；判定以 labwc action 日志为准，不用截图推断。
- 诊断：[notes/desktop-panel-menu.md](notes/desktop-panel-menu.md)。

### DESKTOP-11 Backspace 失效

- 现状：`xfce4-terminal` 里 Backspace 无任何字符；内核 evdev 流已验证 `KEY_BACKSPACE` 正确送达。
- 缺口：故障在合成器/终端一层，未定位。
- 关闭条件：终端里 Backspace 正常删除，不需要改内核。
- 诊断：[notes/desktop-backspace.md](notes/desktop-backspace.md)。

### DESKTOP-12 dbus NoReply 与 `SCM_CREDENTIALS` pid=-1

- 现状：`SO_PEERCRED` 路径已修；dbus 日志里的 `pid=18446744073709551615` 仍来自 `SO_PASSCRED`/`SCM_CREDENTIALS`；会话同步调用 `NoReply`。
- 缺口：AF_UNIX 首包的 `SCM_CREDENTIALS` 为何是 -1 未定位（`SO_PEERPIDFD` 已拿到 pidfd，说明不是取不到 pid）。
- 关闭条件：dbus 日志 pid 正确，同步调用不再超时降级。
- 诊断：[notes/desktop-dbus-credentials.md](notes/desktop-dbus-credentials.md)。

### DESKTOP-13 间歇性 `Failed to set CRTC`（ENOENT）

- 现状：wlroots legacy 后端反复报错，显示起不来；间歇出现。
- 缺口：根因未定位。
- 关闭条件：多次冷启动都能成功设置 CRTC 并出画面。
- 诊断：[notes/desktop-crtc-enoent.md](notes/desktop-crtc-enoent.md)。

### DESKTOP-14 `svcmgr` 强制终止在 AArch64 的验收

- 现状：RV64 dev 镜像上已验证健康超时 → `sig=9` 强停 → 恢复，含负控与 shutdown 审计；AArch64 桌面门禁未复跑。
- 缺口：同一路径在 AArch64 上没有验收；历史上该板的 ufsd 健康探测超时也未解释。
- 关闭条件：AArch64 桌面实例上完成 HANG 注入、强停、恢复与负控，审计干净。
- 来源：原 `known-issues.md` 当前工程队列。
