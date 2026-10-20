# DESKTOP 缺口

对应 V1-DESKTOP。现状见 [current/desktop-minecraft-status.md](../current/desktop-minecraft-status.md)、
[current/known-issues.md](../current/known-issues.md)。

### DESKTOP-01 AArch64 SMP4 竞态

- 现状：观察到两种不同的 SMP 竞态（IPI 风暴后 trap frame 损坏；SIGSEGV 投递期跨任务寄存器交错），当前以 `smp=1` 运行。
- 缺口：竞态未定位，SMP1 不是修复；AArch64 桌面没有多核验收。
- 关闭条件：SMP4 下长时运行无 KERNEL TRAP/PANIC，且用例实际命中共享状态；SMP1 降级不再需要。
- 来源：[current/known-issues.md](../current/known-issues.md)、[instances/xfce-aarch64.toml](../../../instances/xfce-aarch64.toml)。

### DESKTOP-02 RISC-V 桌面交互

- 现状：RISC-V 桌面可启动；MCS 持锁调度、`pgrep` 崩溃与交互复验缺失。
- 缺口：x86_64 的交互验收不覆盖 RISC-V。
- 关闭条件：RISC-V 上完成真实输入/窗口/终端/文件管理器负载并自然关机。
- 来源：[current/known-issues.md](../current/known-issues.md) 当前工程队列。

### DESKTOP-03 应用退出异常

- 现状：COW、FP、TLS 的既有修复不能证明症状消失。
- 缺口：mpv/LuaJIT、tumblerd 与 JVM 的退出异常仍需在干净镜像上复现并定位。
- 关闭条件：三者各自能稳定启动、退出并回收资源；或给出明确的既有缺陷归属。
- 来源：[current/known-issues.md](../current/known-issues.md) 当前工程队列。

### DESKTOP-04 JVM `$ORIGIN` 端到端

- 现状：exec 已从打开文件记录最终路径，`exec-symlink-origin` 通过。
- 缺口：JVM 经符号链接启动的端到端验收缺失。
- 关闭条件：JVM 经符号链接启动并正常工作，路径解析无回归。
- 来源：[current/known-issues.md](../current/known-issues.md) 当前工程队列。

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
- 来源：[current/known-issues.md](../current/known-issues.md) 当前工程队列。
