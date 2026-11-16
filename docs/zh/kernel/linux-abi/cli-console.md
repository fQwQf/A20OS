# CLI 控制台会话

CLI world 的 `/sbin/init` 完成运行时挂载后 exec Alpine BusyBox init。PID 1 负责回收、respawn 和关机；`/etc/inittab` 启动 getty，由它建立 session、控制终端及前台进程组，再执行 `/usr/lib/a20/cli-session`。会话脚本查询控制台尺寸，最后 exec zsh 登录 shell，缺少 zsh 时使用 sh。

继承 stdin/stdout/stderr 不等于已经取得控制终端。原来让 PID 1 直接 exec zsh，导致启动尺寸查询打开 `/dev/tty` 时返回 ENXIO。现在按标准 getty 会话路径使用已有[控制终端接口](tty-jobcontrol.md)，shell 退出后由 BusyBox init 建立新会话。

## 尺寸与退出

`console-size` 经控制终端发送 DSR，stdout 只输出经过校验的尺寸赋值；查询前后保存、恢复 termios。GNU timeout 使用 `--foreground`，使读终端的 dd 保持在前台组。会话脚本把结果导出给登录 shell，交互 `resize-console` 函数通过 helper 的 `--shell` 输出在当前 zsh 更新 COLUMNS/LINES。单独执行外部程序不能修改其父 shell 的环境。

串口字节流没有宿主窗口尺寸事件，手动执行 `resize-console` 可以重新查询。没有终端应答时保留默认尺寸；本轮直接验证的是提供 120×40 DSR 应答的路径，没有将其扩大为所有终端/窗口尺寸的认证。

`/sbin/poweroff` 向 BusyBox init 提交普通关机请求，由 init 停止子进程、同步并调用内核电源接口。

## 当前证据

`build/evidence/v1-closeout-20261116/cli-console/verified/report.json` 为 PASS：RV64 QEMU virt、ABI=both、SMP2，在同一启动中完成两个独立 getty 会话（SID 11、91）。每次均验证 `/dev/tty`、控制会话/前台组、120×40 的 tty 与环境尺寸、查询后的 termios 恢复、当前 zsh 的手动尺寸更新，以及 PID 1 回收孤儿进程。shell 正常 exit 后 respawn 新会话；普通 poweroff 自然退出，ext4/FAT 只读检查均为 0。耗时 27.946 秒。

报告记录冻结内核、基础镜像、所有替换 overlay 文件的哈希/模式及实际脚本。没有重建上游软件或扩大平台矩阵；OpenRC 与其余应用的证据仍由[应用契约表](application-contracts.md)单独跟踪。
