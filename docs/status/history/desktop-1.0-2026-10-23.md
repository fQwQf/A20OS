# 桌面电源菜单、凭据与稳定性验收（2026-10-23）

本轮关闭 DESKTOP-01、02、03、04、05、07、08、09、10、11、12、13，并在 AArch64 SMP4 上复跑
共享调度/内存负载。平台：x86_64/QEMU KVM，4 vCPU、4 GiB，1024×768 virtio-gpu
（pixman）；RISC-V 与 AArch64 为 TCG、4 vCPU。

## 根因与改动

- **面板电源菜单没有任何后端。** `xfce4-panel` 的 actions 插件向
  `org.xfce.SessionManager` 查询 `CanShutdown`/`CanRestart`，没有提供者时退回
  执行 `shutdown --poweroff`；镜像里既没有会话管理器，也没有 `shutdown` 命令，
  也没有 logind（elogind 起不来），所以 Shut Down 静默无效，Restart 因为插件
  默认列表里的 `-restart` 根本不出现。
  新增 `/usr/lib/a20/session-power.py` 在会话总线上提供
  `org.xfce.SessionManager`：`Shutdown`/`Restart` 在回复 D-Bus 调用后走
  `reboot(2)`（内核先 `vfs_system_shutdown`，再关机/重启），`Log Out` 结束
  合成器。`session-start` 在 labwc 之前启动它；`start-desktop-components.sh`
  把 actions 插件的菜单项发布为 `Log Out / Restart / Shut Down`。
- **会话退出后没有用户态系统。** `/sbin/init` 原来是 `exec` 会话，Log Out 或
  合成器崩溃会让 PID 1 直接结束。现在最多重启会话 3 次（间隔 2 s），第 3 次
  仍退出则关机。
- **dbus 自己的 pid 是 -1。** dbus 1.16 为当前进程取 pidfd 后用
  `/proc/self/fdinfo/<fd>` 的 `Pid:` 行解析 pid；A20OS 的 fdinfo 只有 pos/flags/ino，
  解析失败后 dbus 打印 `DBUS_PID_UNSET`。fdinfo 现在对 pidfd 输出
  `Pid:`/`NSpid:`，日志前缀恢复为真实 pid。
- **mpv 的 Lua 规避不再需要。** 匿名 fault-around 窗口在 VMA 被并发合并时把良性
  身份变化判成失败，是 mpv/LuaJIT、tumblerd、JVM 那条“指针字段变 NULL”的根因
  （修复与 `smoke-mtcorrupt` 门禁已在树上）。`mpv.conf` 删除
  `load-scripts=no`/`osc=no`/`ytdl=no`，恢复内置脚本。
- **ext4 的 depth-1 extent 与关机 orphan（跨领域窄修）。** full profile 默认
  改用 Rust extent 事务（C root-trim 只接受 depth-0，桌面写出的大文件无法在
  unlink 后回收）；`ext4_shutdown_clean()` 在持久化 CLEAN 前回收残留 orphan，
  因为被删除但仍被 vnode 缓存的 inode 会留到会话结束。两轮桌面写负载后
  宿主 `e2fsck -fn` 干净。
- **`pt_unmap()` 的页表回收提示未初始化（跨领域窄修）。** 下降循环只填
  `idx_path[1..ROOT]`，回收循环第一层用的 `idx_path[0]` 从未赋值；`pt_table_empty()`
  只用它做“非空”快路径提示，所以症状是空页表可能不被回收，同时 riscv64 的
  `-Werror=maybe-uninitialized` 让整条构建失败。补上 `idx_path[0] = leaf_idx`，
  riscv64 镜像恢复可构建；x86_64 SMP4 上 `mm_stress`/`vfs_stress`/`mtcorrupt_test`
  复跑通过。

## 验证

在 x86_64 KVM 桌面上（当前提交、独立工作树）：

| 检查 | 结果 |
| --- | --- |
| 冷启动 CRTC | 10 次冷启动全部出画面，`Failed to set CRTC` 0 次 |
| 壁纸 | 冷启动后 1024×768 截图 3332 色、黑像素 0.01%，`swaybg` 只启动一次，无重启看门狗 |
| 面板菜单项 | Applications → Terminal Emulator 启动终端并关闭菜单 |
| Backspace | 终端里输入 `echo ab<BS>c<Enter>`，屏幕与输出均为 `ac` |
| 电源菜单 | 三个条目齐全；Restart 触发 `[SD] RESTART CLEAN entry` 后 QEMU 复位退出；Shut Down 打印 `[SD] POWER_OFF entry`、`audit errors=0` 后关机；Log Out 结束会话，init 重启后桌面恢复 |
| JVM `$ORIGIN` | `/root/java-link -> .../bin/java`：`-version` 正常，经链接编译并运行 Java 程序正常 |
| dbus pid | 日志前缀 `[session uid=0 pid=55 pidfd=4]`；`/proc/self/fdinfo/<pidfd>` 含 `Pid:` |
| JVM 退出码 | `java -version` 20/20 返回 0 |
| mpv | 20/20 次带 Lua 脚本运行无崩溃；40 次进程 churn 后 unusual=0 |
| tumblerd | D-Bus 缩略图请求成功，产出缓存 PNG 后正常退出，无 FATAL |
| 稳定性 | 30 分钟桌面长跑（60 次采样）0 FATAL / 0 SIGSEGV / 0 PANIC；`mtcorrupt_test` 在 x86_64 SMP4 PASS |
| AArch64 SMP4 | `sched_stress`、`proc_stress`、`futex_stress`、`mm_stress`、`scm_stress` 全部 PASS，约 30 分钟无 TRAP/PANIC，调度器违规为 0 |
| AArch64 SMP4 桌面 | TCG 4 vCPU 冷启动到完整桌面（`[SMP] 4/4 configured CPUs online`），10 分钟以上无 panic；该轮 udev input worker 超时导致键盘未被 libinput 接管 |
| RISC-V 桌面 | TCG 4 vCPU 冷启动；Applications → Terminal 输入 `echo a20-riscv` 与 Backspace 测试；Thunar 打开 `/root`；电源菜单 Shut Down 打印 `[SD] POWER_OFF entry` 后关机 |
| RISC-V 关机拒绝对照 | 修复前 `mntns_shutdown_begin()` 返回 `-EBUSY`，`[SD] poweroff refused: filesystem shutdown failed (-16)`，QEMU 不退出；加入 RETRY_* 语义的有界重试后同一操作关机 |
| `smoke-mtcorrupt` | riscv64 `NR_CPUS=4 CONFIG_SLAB_DEBUG=1` 构建通过并 PASS（此前被 `mm.c` 的未初始化告警挡住） |
| ALSA 修复 | `snd_pcm_open("hw:0,0")`/`"default"` 都成功；OpenAL（`drivers = alsa`）source 进入 PLAYING/STOPPED 且无错误；mpv `--ao=alsa` 播放 440 Hz；QEMU wav 录到 441 Hz、非静音 |
| Minecraft 音频 | LWJGL OpenAL 在 "ALSA Default" 上初始化，`Sound engine started`；标题音乐被 QEMU wav 录下（33 秒、非静音、主频约 185 Hz） |
| 全屏 | `xfce4-terminal --fullscreen` 与 Minecraft（XWayland，F11）都覆盖整个 1024×768 输出 |
| `smoke-audio-userspace` | x86_64 上 PASS（`check_wav_pcm: PASS rate=44100 channels=2 frames=218824`） |
| 桌面写盘 | full profile 改用 Rust extent 事务并在关机前回收残留 orphan；两轮写负载（200 文件 + 6 段稀疏文件 + 缩略图/xfconf）后正常关机，宿主 `e2fsck -fn` 两轮都干净 |
| FS 回归 | 同一内核上 `vfs_stress`、`mm_stress`、`mtcorrupt_test`、`scm_stress` 全部通过，0 panic |

负控：旧的 x86_64 内核上运行新增的 fdinfo 断言得到
`SCM_STRESS: FAIL peer pidfd fdinfo must name process`；当前内核通过。

## 未完成

- AArch64 桌面在 SMP4 冷启动并渲染，但该轮 udev 的 input worker 超时导致键盘
  未被 libinput 接管；输入/窗口负载只在 x86_64 与 riscv64 上验收。
- 语音聊天使用的 16 kHz 采集设备未开放（ALSA 层只报告 48 kHz 播放/采集）；
  Minecraft 记录 `failed to set sample rate near 16000` 后跳过语音。
- Minecraft 帧率目标/对照（DESKTOP-06）、AArch64 `svcmgr` 强停验收
  （DESKTOP-14）仍未完成。
