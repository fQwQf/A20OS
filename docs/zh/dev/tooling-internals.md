# 构建与宿主工具技术参考

本页面向维护构建接线、回归夹具和宿主工具的开发者。执行构建见 [构建指南](build.md)，配置串口、运行实例与查看失败信息见 [实例手册](instances.md)。

## 实例解析与注册表 {#instance-parsing}

`tools/a20` 读取实例、校验配置、推导 Makefile 变量并调用 make。未写的字段不传递变量，由 Makefile 取默认值；门禁逻辑由 Makefile 维护，能力矩阵与 MCU profile 源文件清单由`components/trim.toml` 维护。`components/trim.mk` 是生成物，实例校验与 `a20 trim` 读取同一份 TOML。修改注册表时必须同步生成并校验，避免实例层与构建层各维护一套默认值。`[target]` 中的串口、复位和 expect 字段由 a20 的 dataclass 与控制台动作直接消费，不经过 make recipe。导出层只应传递有实际 make 消费者的字段，否则 `show-vars` 会误报构建层拥有的配置。

QEMU 参数通过 `_qemu_argv` 查询，生成与执行的分工见 [参数接线](#qemu-arguments)。

## 目标运行库 {#target-runtime}

`rust-src` 不是可选项：riscv32 是自定义 JSON 目标（`tools/rust/targets/riscv32imafdc-a20.json`），构建入口用`-Z build-std=core` 现场编 core，而 core 来自这个组件。构建入口自己会设`RUSTC_BOOTSTRAP=1`，所以 stable 工具链够用。

## ARM32 时间 ABI {#arm32-time-abi}

ARM32 musl 的 `time_t` 是 64 位，直接 syscall 回归 `timer_edge` 因此使用 `timer_settime64` / `timer_gettime64` 编号和同宽的 `itimerspec` 布局；不要把 32 位 `timer_*32` 编号与 64 位结构体混用。

对应夹具是 `user/cmds/stress/timer_edge.c`。它直接调用 syscall，不能依赖 libc 包装器转换时间结构；调整 syscall 编号时必须同时核对结构宽度。

## 并发构建契约 {#concurrent-builds}

顶层构建可能同时从磁盘镜像与 RAMFS guest kernel 进入用户构建。`tools/stamps.py` 会按共享的 `user/build/<arch>[-nommu]` 输出根使用跨进程锁，并在拿锁后重新检查 stamp；因此不同 `OPT`/profile 或 user/native stamp 的独立 make 进程不会并发清理或写入同一目录。锁文件位于该目录的父目录，不会随变体 clean 一起删除。

## 串口传输与平台编码 {#serial-transport}

串口传输使用标准库的 `termios`、`fcntl` 与 `select`；`tools/a20` 的 PEP 723 头声明`dependencies = []`，因此传输实现不引入 pyserial。波特率编码由宿主 `termios` 提供，不维护另一份编码表：例如 Linux 的 `B115200` 为 `0o10002`，BSD 的编号不同。

具体实现位于 `tools/cli/a20_console.py` 的 `SerialTransport` 和 `_baud_code`；波特率不受支持时报告宿主 `termios` 提供的可用值。

## 同实例互斥 {#instance-exclusion}

`a20 test` 在构建前取得 `.kernel-build/smoke/<name>.lock` 的非阻塞 `flock`。锁覆盖构建与运行会话；推迟到构建之后取锁会让两个进程先写入同一 `BUILD_DIR`，即使随后拒绝其中一个会话，也已无法保护构建产物。

获取锁失败向用户报告 `InstanceBusy`，退出码为 1，诊断包含锁路径。

## QEMU 参数生成与执行 {#qemu-arguments}

make 拥有每一个 flag。`$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)` 在 make 里展开，`tools/qemu.py` 读到这个结果并负责执行（build → verify → exec），`a20 run` / `debug` 走的是同一条路。所以启动顺序里不再有 shell 配方，而命令行只有一个来源。

`_qemu_argv` 只负责打印这个结果，**因此必须没有前置依赖**：一旦它带上依赖，"取一下命令行"就会先把整个世界重新构建一遍。查询命令行要用 `make _qemu_argv`，不要加 `-n`。加了会打印出 `printf` 本身，而不是它输出的命令行。

`a20 test` 与 `run` / `debug` 复用该参数入口；`machine.extra_qemu` 经 `EXTRA_QEMU`参与参数生成。该字段是字面量 argv 数组：派生时保留每项边界并保护 `$`，查询配方将完整命令安全地作为数据打印，Python 再解析 argv。因此多项内核参数须放在同一个 `-append` 值里，混合引号或字面量命令替换不应被 Make/查询 shell 执行。实例层不复制一份 QEMU 默认参数逻辑。

## 宿主端口探测 {#host-ports}

声明的转发端口在启动前实际 bind 一次，不能只检查 `/proc`：QEMU 不设置 `SO_REUSEADDR`，实际 bind 才能确认该地址和端口的可用性。端口被占用时复用内存/CPU 的资源等待机制，`--no-wait` 与 `--wait-timeout` 保持同样的失败语义。

## world 镜像检查 {#world-images}

`run` 先完成内核、软件包与 world 镜像构建，再启动 guest；`build` 执行构建后返回。`mkrootfs.py` 每轮按 world 清单重新生成镜像，前一轮 guest 写入不作为下一轮输入。 world 使用无日志 ext4，QEMU 被直接终止后的脏状态不代表生成器写坏镜像。`a20 run` 通过 `check_image=inst.rootfs.world is None` 对 world 实例跳过启动前 fsck。
