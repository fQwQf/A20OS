# 命名宿主实例与交互控制台

已进行完整构建和 QEMU 实测；具体通过项和未通过项见 实现状态。推荐从 [使用指南](03-usage.md) 开始。

## 创建与连接

```sh
tools/a20 run hyp-host-riscv64 --name lab-a --detach
tools/a20 run hyp-host-riscv64 --name lab-b --detach --no-build
tools/a20 running
tools/a20 attach lab-a
```

模板 `instances/hyp-host-riscv64.toml` 选择 RISC-V、ABI=both、1 CPU、1 GiB RAM、TCG 与 H/Sstc。现有镜像打包规则提供 hypvm 及 guest ELF。模板还启用 `machine.vm_manager=true`，命名运行时接入专用 virtio-serial 控制端口并自动启动 manager。双 guest 管理见 [07-guest-supervisor.md](07-guest-supervisor.md)。也可在 A20 宿主 shell 手工使用：

```sh
hypvm --managed -k /bin/boot/guest-kernel-ramfs.elf
```

`--managed` 经每 VM 队列转发 stdin/stdout，并在 guest IRQ 或输出后返回宿主，供调度和用户态 I/O 处理。独立 guest 的输入应来自独立 PTY 或管道；多个进程共用同一个 stdin 会由宿主终端层竞争输入。

`Ctrl-]` 断开外层控制台，实例继续运行。控制台只有一个写者，另一个 attach 会拒绝；近期输出保存在运行目录的 `console.log`。attach 不负责自动输入 shell 命令。

## 生命周期

```sh
tools/a20 pause lab-a
tools/a20 resume lab-a
tools/a20 stop lab-a
tools/a20 destroy lab-a
```

pause/resume/stop 通过私有 QMP socket 操作对应 QEMU，stop 是退出 QEMU，不是请求 guest 文件系统优雅关机。需要优雅关机时先在 guest/宿主控制台执行 poweroff。destroy 只删除已停止实例的运行目录和磁盘；进程身份通过 PID 加 `/proc` starttime 核对，避免 PID 回收误判。

目录位于 `$XDG_RUNTIME_DIR/a20-runs/<name>`，未设置时为 `/tmp/a20-<uid>/a20-runs/<name>`，属于本用户且权限私有。目录是临时运行状态，需长期保留的 guest 数据应在销毁或主机重启前另行保存。

每个可写 drive 先 flatten 为私有 qcow2 base，再创建私有 overlay，因此重编译模板磁盘不会修改运行中 guest 的 backing。此步骤需要磁盘空间并增加启动时间。名称独立于模板名，同一模板可创建多份。模板显式配置的端口仍须互不冲突；默认网卡获得按名称派生的 MAC，显式 MAC 保持模板值。

当前命名实例只接受 headless 基础镜像，拒绝 world/GUI 和无法安全改写的自定义 QEMU 块设备、chardev 等参数；构建变量需放入 manifest。`--dry-run` 只打印计划，`--no-build` 要求产物已经存在。

## 加速器

`[machine] accel = "tcg"` 或 `"kvm"` 会派生明确的 `QEMU_ACCEL`。选择 kvm 不配置 TCG fallback；这指开发机上的外层 QEMU 加速器，不表示 A20 已实现 `/dev/kvm`。A20 内核虚拟化设计与特性实现参见 [00-design.md](00-design.md) 与 [02-roadmap.md](02-roadmap.md)。
