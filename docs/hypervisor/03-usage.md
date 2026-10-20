# Hypervisor 使用指南

当前入口是 RISC-V H/Sstc 宿主上的 `hypvm` 和 `a20-vmd`。Native 与 Linux 扩展 ABI 包装同一个 VM 核心。其它架构后端、A20 `/dev/kvm` 和 QEMU 作为 A20 内部 VMM 的兼容层尚未实现。实测能力和限制见 [实现状态](../status/current/hypervisor-implementation-status.md)。

## 构建和启动宿主

在 Linux 开发机安装 Python 3.11+、GNU make、RISC-V GCC/binutils、QEMU system-riscv64、qemu-img，以及镜像工具 mtools、dosfstools、e2fsprogs、xorriso。交叉编译器名称和更多依赖见仓库构建指南。

```sh
tools/a20 build hyp-host-riscv64 -- -j4
tools/a20 run hyp-host-riscv64 --name lab --detach --no-build
tools/a20 attach lab
```

该模板使用 ABI=both、1 CPU、1 GiB RAM、TCG，显式启用 H 和 Sstc。构建会同时打包 RAMFS guest 内核和用户程序。命名运行创建独立磁盘 overlay、宿主 UART 和管理 socket；宿主日志应出现 `A20VMD: READY protocol=2`。`Ctrl-]` 断开控制台，宿主继续运行。

资源不足时命令会等待。可设置 `A20_WAIT_TIMEOUT=60 A20_PREFLIGHT_TIMEOUT=60` 限定两层检查的等待时间。确认空间足够后可设置 `A20_MIN_DISK_MB=2048`；内存、并发和其它预算见 `tools/a20 run --help`。不要通过降低预算启动超出实际可用资源的实例。

## 在同一宿主创建两个 guest

```sh
tools/a20 vm start a20-ramfs-riscv64 --host lab --name guest-a
tools/a20 vm start a20-ramfs-riscv64 --host lab --name guest-b
tools/a20 vm list --host lab
tools/a20 vm console guest-a --host lab
```

另一个终端连接 `guest-b`。等 shell 提示符出现后输入 `echo hello`。每台 guest 有独立 stdin/stdout 队列；`Ctrl-]` 断开后可重新执行 console 命令。START 成功表示管理器接受创建，是否启动成功还需查看 console/list。默认 guest 为 128 MiB、1 vCPU，无磁盘和网络，仅 RAMFS。

```sh
tools/a20 vm pause guest-a --host lab
tools/a20 vm resume guest-a --host lab
tools/a20 vm stats --host lab
```

暂停/恢复是 VMM 进程控制；用 list 等待实际状态。正常结束在 guest shell 输入 `exit`，待 list 显示 exited、退出码 0 后销毁：

```sh
tools/a20 vm destroy guest-a --host lab
tools/a20 vm destroy guest-b --host lab
tools/a20 stop lab
tools/a20 destroy lab
```

强制终止使用 `tools/a20 vm stop NAME --host lab`，确认 exited 后再 destroy。外层 `tools/a20 stop lab` 会退出整个 QEMU；临时磁盘数据在 destroy 时删除。

## 可重复的真实场景检查

```sh
python3 tools/hyp_pair_probe.py --host lab --timeout 120 --report /tmp/a20-pair/report.json
```

脚本创建两台 guest，等待真实 shell 提示符，执行不同命令，检查控制台隔离、重连、暂停隔离、正常关机、记录销毁，以及 VM/vCPU/映射/页表计数回到静止基线。JSON 和每台 guest 的日志写到 report 旁边。一次通过不能代替长稳和竞态测试。

## 单进程和 SMP 配置

在 A20 宿主 shell 可直接运行：

```sh
hypvm --managed -k /bin/boot/guest-kernel-ramfs.elf -m 128
hypvm --help
```

直接运行时 stdin/stdout 属于当前终端；同时运行多个进程应提供独立管道或 PTY。管理器已经代办这些隔离。

仓库保留 `hyp-host-riscv64-smp2` 宿主和 `a20-ramfs-riscv64-smp2` guest profile。必须先构建对应 SMP 宿主，使打包的 RAMFS 内核也支持 2 CPU，再使用对应 profile；`vcpus` 参数本身不能把单 CPU 内核变成 SMP 内核。SMP 的验收状态以 [实现状态](../status/current/hypervisor-implementation-status.md) 为准。

## 排查

- 找不到管理器：查看宿主 `console.log`，确认模板启用了 `vm_manager=true`，使用命名运行，且出现 READY；guest console 与宿主 attach 是不同入口。
- `errno=95`：CPU 缺 H/Sstc，或 firmware 未开放必要的虚拟计时器能力。无 H 时拒绝创建是预期行为。
- 创建后马上退出：读 guest console 中的 `HYPVM: FAIL` 和 list 退出码；检查 kernel 路径与 RAM 大小。
- 管理请求超时：保留宿主日志和 guest 日志。无法通过 stop guest 恢复时，停止外层宿主并重新创建。当前实现仍需故障和长稳验收。

协议与生命周期细节见 [宿主运行目录](06-host-runtime.md) 和 [guest supervisor](07-guest-supervisor.md)；未来平台目标见 [总体设计](04-platform-design.md)。
