# Guest supervisor 与双 guest 管理

已完成构建并实际运行双 guest 场景；单次完整流程通过，重复运行和 SMP 仍有失败。当前为实验功能，详见 实现状态。

```sh
tools/a20 run hyp-host-riscv64 --name lab --detach
tools/a20 vm start a20-ramfs-riscv64 --host lab --name guest-a
tools/a20 vm start a20-ramfs-riscv64 --host lab --name guest-b
tools/a20 vm list --host lab
tools/a20 vm console guest-a --host lab
# 在另一个终端：
tools/a20 vm console guest-b --host lab
```

`vm-profiles/a20-ramfs-riscv64.toml` 选择实际实现的 riscv64/hypvm/native、单 vCPU、128 MiB，以及镜像内 `/bin/boot/guest-kernel-ramfs.elf`。当前不接受其它 backend、guest SMP、任意可执行文件或 shell 字符串。

`Ctrl-]` 脱离某个 guest console，VMM 继续运行；再次执行 console 会使用对应 manager epoch、guest generation 和持久游标接续未读输出。每 guest 同时只有一个交互写者。manager 保留最近 64 KiB 输出，超出时显式报告历史缺口；输入限长并按实际接受字节数确认，客户端保留未确认输入。本地日志在宿主 runtime 的 `guests/<name>.log`。

```sh
tools/a20 vm pause guest-a --host lab
tools/a20 vm resume guest-a --host lab
tools/a20 vm stop guest-a --host lab
tools/a20 vm list --host lab
tools/a20 vm destroy guest-a --host lab
tools/a20 vm stats --host lab
```

pause/resume/stop 控制 supervisor 持有的 VMM 子进程。pause 状态先为 pausing，waitpid 观察到 stop 后才为 paused；stop 先恢复停止的进程再发送 SIGTERM，逾期发送 SIGKILL。它们是进程控制，并不代表 guest 文件系统优雅关机。需要正常关机时在 guest shell 执行 exit/poweroff。只有已停止的 VMM 可以 destroy，输出排空后才报告 exited。控制命令返回 accepted，实际状态由 list 查询。

同名 START、相同配置会返回现有记录的 generation；不同配置会拒绝。退出记录需要 destroy 后才可用同名重建。控制/IO 请求同时携带 manager epoch 与 guest generation，旧控制台不能误操作同名新 guest。

## 传输与进程归属

`[machine] vm_manager=true` 只用于命名的 RISC-V Linux/both 基础镜像宿主。runner 添加独立 virtio-serial PCI function、port 0 virtconsole 和私有 `control.sock`，bootargs 添加 `a20.vmd=1`。宿主 UART 与控制端口分离。驱动从 probe 起关闭控制端口的 UART mirror，并拒绝重新开启，消除 daemon 启动前把协议字节当 shell 输入的窗口。当前复用已有单 port 驱动，没有假定 MULTIPORT 支持。

PID 1 根据显式 bootarg 启动 `/bin/a20-vmd`。daemon 只以 root 运行，通过文件锁限制单实例，等待 `/dev/vport0`，随后输出 `A20VMD: READY`。每个子 VMM 通过独立 stdin/stdout pipe 执行固定 `/bin/hypvm --managed`，没有 shell 拼接；其它子进程的 pipe fd 会关闭。PR_SET_PDEATHSIG=SIGKILL 和 fork 后 parent 身份重查避免 manager 崩溃遗留 guest。manager 崩溃后没有进程/VM 恢复，重新启动生成新的 epoch。

daemon 最多保留 8 个 guest 记录，运行中 guest 的 RAM 请求合计不超过 512 MiB。该预算不等于完整物理内存限制：内核页表、VMM、pipe 和设备内存尚需纳入统一资源模型。当前宿主模板 1 GiB/1 CPU，两个 guest 按时间片运行；宿主 SMP 的不同 VM 并行路径尚未验收，同 VM SMP 已加入代码，实测状态见实现状态页。

## Wire v2

每个消息使用 0x7e 定界，0x7e/0x7d 通过 0x7d+xor 0x20 转义。完整解码后的消息为 24 字节 little-endian header：magic A2VM、version u16、op u16、request id u64、payload length u32、CRC32 u32。CRC 覆盖前 20 字节 header 和 payload；payload 上限 4096。完整边界和 CRC 校验避免断连残留或 payload 中的 magic 被误解释为请求。

HELLO 返回版本、记录上限、RAM 预算和 manager epoch；其余请求以 epoch u64 开始。guest 定位使用零填充的 32 字节 name，以及控制/IO 的 generation u64。响应 op 设置 0x8000，payload 首字段为 errno 风格的有符号 status i32。START 在 name 后携带 RAM MiB u32、vCPU 数 u32 和 kernel 路径 256 字节。LIST 记录为 72 字节，包含 name、pid、state、exit code、RAM MiB、generation、output cursor、vCPU 数和保留字段。

manager 缓存最近 32 个请求的完整原文与响应，丢失回复的同一 request id 重试不会重复产生副作用；同 id 不同原文会拒绝。缓存不是崩溃持久化，也不是无限期幂等。客户端对 ready 之后的事务最多尝试两次；重试期间 epoch 改变会中止。每次 RPC 是短连接，宿主文件锁在多个独立终端之间串行复用 control socket。管理 Unix socket 由宿主私有 runtime 目录保护，CRC 不提供密码学认证；该管理通道只对宿主管理员开放。

## 运行门禁

```sh
python3 tools/probes/hyp_pair_probe.py --host lab --report /tmp/a20-pair/report.json
```

脚本会实际创建两台 guest，等待各自 shell，用输入回显中不存在的拼接 token 检查命令执行，检测串线，检查重连和暂停隔离，再等待正常关机与输出排空、销毁记录，并检查 VM/vCPU/mapping/table 计数回到静止基线。失败日志与 JSON 证据保留。该脚本已实际运行，结果及边界见 实现状态。

RISC-V trap 控制中的 VS fence、hvip 和保留位依据[官方 Hypervisor 规范](https://docs.riscv.org/reference/isa/priv/hypervisor)及[Sstc 规范](https://docs.riscv.org/reference/isa/v20250508/priv/sstc.html)：VS-stage 更新使用 HFENCE.VVMA；hvip 的软件注入与 vstimecmp 产生的 timer 信号相互独立。定时器、SMP 和远程控制的实测范围以实现状态页为准；尚无完整外部中断注入和真机验收。
