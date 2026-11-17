# 内核热切换（kexec）

A20OS 的内核热更新采用两阶段 kexec：运行中的系统先暂存新内核，再在不经过固件复位的情况下切换到它。持久 rootfs 保留，新内核重新执行 init 和服务启动链。

这不是进程透明的 live patch。旧进程、句柄、锁、驱动运行时状态和网络连接不跨内核存活；切换期间服务会短暂中断。最终跳转后没有 A/B 自动回滚，启动失败仍需要旧启动介质。

## 分层

- `kernel/core/kexec.c`：权限、加载互斥、执行状态机、文件系统关闭、设备静默和架构钩子。
- `kernel/mm/elf_core.rs`：ELF 解析、地址溢出/重叠检查、staging ledger、原子替换和失败回滚。
- `kernel/abi/linux/`：`kexec_load`、`kexec_file_load`、`reboot(KEXEC)` 的 Linux 参数适配。
- `kernel/abi/native/`：`system_kexec` 的 Native 参数复制、UAPI 和 errno 映射。
- `kernel/arch/<arch>/`：各架构后端的物理布局验收、boot CPU 绑定、SMP 停核和最终 MMU/入口切换（RISC-V 32/64 共享 `kernel/arch/common/riscv/kexec.c`）。
- `kernel/drivers/core/`：terminal shutdown 能力预检和逆序设备静默。

Linux 与 Native 共用同一份镜像 ledger 和执行实现，没有两套更新状态机。

## 加载与执行

加载是私有事务。ELF 的每个 `PT_LOAD` 都展开为物理目标段；文件数据经 `read_at` 回调直接流进独立页链（不经过整镜像缓冲），`p_memsz - p_filesz` 部分清零。入口由 `e_entry` 和包含它的段转换为物理地址；e_entry 已经落在某段物理地址区间时（如 AArch64 以物理入口交付 QEMU）按原值接受。只有全部段成功后才原子替换旧暂存镜像；失败会释放新事务并保留旧镜像。

执行依次完成：

1. 占用 `EXECUTING` 状态，拒绝并发加载、撤销和执行。
2. 检查所有绑定驱动是否提供 terminal shutdown。
3. 校验真实 RAM ranges、DTB、暂存页和控制页，分配最终 trampoline。
4. 保存调用任务的 affinity，并绑定、迁移到 boot CPU。
5. 停核其他 CPU：各架构后端用自己的机制（SBI HSM / PSCI / AP hlt / 固件复位桩，见下文矩阵）。
6. 调用 `vfs_system_shutdown()` 同步并关闭文件系统。
7. 调用驱动的 terminal shutdown，停止请求和 DMA、复位设备。
8. 在旧内核文本中的入口关闭或切换 MMU，跳到独立物理 trampoline，复制段并清零 BSS 尾。
9. 以该架构的启动约定进入新内核（hart ID + DTB、DTB in x0、Multiboot info、mailbox 等）。

暂存、校验、设备能力预检失败不会影响运行系统。停核或 VFS 关闭失败会尝试恢复 secondary hart 和调用任务 affinity。最终复制和跳转之后不再提供旧内核恢复。

## Linux ABI

```c
int fd = open("/bin/boot/kernel-next.elf", O_RDONLY);
syscall(SYS_kexec_file_load, fd, -1, 0, NULL, 0);
close(fd);
reboot(RB_KEXEC);
```

`kexec_file_load(..., KEXEC_FILE_UNLOAD)` 撤销暂存镜像。当前不接受 initrd 或替换命令行；不支持的 flags 和 crash 模式会明确失败。

## Native ABI

`A20_SYS_system_kexec = 0x0A04` 支持：

- `A20_KEXEC_LOAD_IMAGE`：从用户 ELF 缓冲区加载；
- `A20_KEXEC_LOAD_SEGMENTS`：从固定宽度物理段描述符加载；
- `A20_KEXEC_EXECUTE`：执行已暂存镜像；
- `A20_KEXEC_DISCARD`：撤销镜像；
- `A20_KEXEC_STATUS`：查询状态、入口、段数和后端能力。

SDK 声明位于 `user/liba20rt/a20_kexec.h`，内核 UAPI 位于 `kernel/include/uapi/a20/kexec.h`。加载、执行和撤销要求 `CAP_SYS_BOOT`（保留 root 兼容规则）。

## 命令

开发 FAT rootfs 挂载在 `/bin`，因此示例路径带 `/bin` 前缀：

```sh
kexec -l /bin/boot/kernel-next.elf  # Linux ABI：只暂存
kexec -e                           # 执行
kexec -d                           # 撤销
kexec /bin/boot/kernel-next.elf    # Linux ABI：暂存并执行

native-kexec-rv                    # Native 人格程序：加载、查询并执行
```

Linux 人格任务不能直接把 Native 编号当 Linux syscall 使用；Native 示例由 Native 启动协议运行，避免把两种人格的编号空间混用。

执行前应停止非必要写入并保留旧启动介质。新内核启动后需检查版本、存储、网络、服务以及持久数据。

## 驱动契约

`driver_t.shutdown` 是 terminal 回调，不等于 `remove`：不注销设备、不遍历释放 registry。没有回调的绑定驱动会让执行在停核前失败。VirtIO block/net 停止新请求和 IRQ、处理在途状态并复位设备；无硬件动作的驱动也必须显式提供 no-op，不能以省略回调表示安全。

## 当前平台边界

后端按 架构 + 板型 + MMU 三件套启用，各自携带独立的停核与交接实现，`machine_kexec_backend_id()` 报告当前后端 ID：

| ID | 架构 | 板型 | 停核机制 | 交接要点 |
|----|------|------|----------|----------|
| 1 | riscv64 | qemu-virt-riscv64 | SBI HSM `hart_stop` + 状态轮询 | `satp` 切启动页表，trampoline 关 MMU 后按物理地址复制 |
| 2 | x86_64 | qemu-virt-x86_64 | reschedule IPI + `cli; hlt` | 专用 PML4/PDPT/PD（4 GiB 恒等映射）放在帧池页里：旧内核 `.bss` 里的启动页表会被新内核 BSS 清零覆盖，不能复用；Multiboot 信息块同样落在 BSS 目标区，prepare 时快照进帧池页 |
| 3 | aarch64 | qemu-virt-aarch64 | PSCI `CPU_OFF` + `AFFINITY_INFO` 轮询 | 专用 L0/L1 页表（恒等 + 高半别名，kwx finalize 把启动 L1 劈成 PXN 块后恒等别名不可执行）；先清 WXN 再换 TTBR0；trampoline 按 CTR_EL0 行宽 `dc cvac` 清到 PoC，关 MMU/缓存后以 x0=DTB 进 `_start` |
| 4 | loongarch64 | qemu-virt-loongarch64 | reschedule IPI 后跳回固件复位桩（0x1c000000）等 mailbox | 恒等 DMW 下无需换页表；trampoline 复制完切 CRMD.DA；新内核经 IOCSR mailbox 0 正常拉起副核 |
| 5 | arm32 | qemu-virt（armv7-a） | PSCI `CPU_OFF` + `AFFINITY_INFO` 轮询 | 专用 16 KiB 短描述符 L1（RAM 恒等段 + 高半别名）；trampoline 按 CTR 行宽 DCCVAC 清到 PoC，关 MMU/缓存后以 r2=DTB 进 `_start` |
| — | riscv32 | qemu-virt-riscv32 | SBI HSM（同 ID 1 的共享后端） | 与 RV64 共享 C 后端（`kernel/arch/common/riscv/kexec.c`），32 位 trampoline 用 4092 字节链节点布局，Sv32 `satp` |

RV64 的 `smoke-kexec-handoff{,-native-smp2}` 之外，每个新后端各有独立验收：

```sh
make smoke-kexec-handoff-x86_64
make smoke-kexec-handoff-aarch64
make smoke-kexec-handoff-loongarch64
make smoke-kexec-handoff-riscv32
make smoke-kexec-handoff-arm32
```

每个用例都真实构建 kernel.elf、复制进可丢弃的 FAT 副本 `/boot/kernel-next.elf`、首次启动写持久标记、`kexec_file_load + reboot(KEXEC)`、二次启动读标记并正常关机，断言两次 `[SMP] 2/2 configured CPUs online`。arm32 用例还需要 `arm-linux-gnueabihf-gcc` 在 `PATH` 中（Debian 系：`gcc-arm-linux-gnueabihf` 包，或无 root 时 `apt-get download` 后 `dpkg -x` 解包再加 `PATH`）；另外 qemu-virt-arm32 的 TCG PSCI 不实现 `SYSTEM_OFF`（`hvc` 返回 NOT_SUPPORTED，机器无 EL3 可走 `smc`），guest 的 `poweroff` 停在 `arch_halt()`，QEMU 由 smoke 框架超时回收——这是预先存在的平台缺口，与 kexec 无关，该用例的验收因此只看日志断言、不要求 QEMU 正常退出。NOMMU、MCU 和物理板型不自动获得支持，仍需各自的物理内存保留图、停核、缓存和启动交接验收。

## 服务级检查点/恢复（CRIU-lite）

kexec 切换本身不保留进程。在此之上提供单任务检查点/恢复：切换前把服务冻结并序列化到持久文件，新内核启动后从镜像恢复，服务从被冻结的指令处继续运行，内存内容和文件描述符不丢失。

```sh
svcdemo &                                        # 目标服务
svcctl checkpoint $(cat /bin/svcdemo.pid) /bin/svc.ckpt
kexec /bin/boot/kernel-next.elf                  # 热切换
# 新内核里：
svcctl restore /bin/svc.ckpt &                   # 恢复方变成服务本身
```

### 分层

- `kernel/proc/checkpoint.c`：经 ptrace 机制冻结目标任务（attach + interrupt + 停点确认），快照 VMA 表、寄存器（`arch_ptrace_export_regs`）和 fd 表（`vfile_t` 的 path/offset/flags），写 A20CKPT1 镜像。
- `kernel/proc/ckpt_restore.c`：restore-as-exec——校验镜像、为**调用者**构建替换地址空间（逐页填充）、重建 fd 表（按路径重开并恢复偏移）、切换 mm 并灌入完整寄存器组。成功时不返回。
- `kernel/proc/ckpt_image.rs`（Rust）：镜像头/VMA 校验、恢复陷阱帧装配，以及 ptrace/coredump 共用的寄存器帧导出导入。rv64 帧槽 `x[0]` 不是 GPR 而是 satp 令牌暂存槽，Rust 把它建模为独立的 `addrspace_token` 字段：导出恒报硬件语义（x[0]=0，内核令牌不泄漏给调试器/coredump/镜像），导入只写体系结构寄存器、永不触碰令牌槽（构造的 regset 无法把任务引到别的地址空间）。这是开发中真实踩过的 bug 类别（导入器覆盖令牌导致恢复任务跑在旧任务的页表上），现在由类型系统根除；宿主机测试 `make test-ckpt-image` 含该回归用例。
- ABI 薄包装：Linux `SYS_a20_task_checkpoint=923`、`SYS_a20_task_restore=924`（fd 语义）；Native `A20_SYS_task_checkpoint=0x0217`、`A20_SYS_task_restore=0x0218`（路径语义）。

### 契约与限制

- 仅单线程任务；遇多线程、VM_SHARED 映射、socket/管道/channel 等无路径 fd 即明确报错（`-EOPNOTSUPP`），可用 `A20_CKPT_F_SKIP_UNSUPPORTED_FDS` 把不可恢复 fd 记为已关闭。
- socket/TCP 连接、channel、定时器、信号状态、命名空间状态不跨内核存活——这是物理限制，不是实现缺口。
- 恢复后的任务使用**恢复者**的凭据；镜像中的 uid 只做一致性校验（不同 uid 且非 root 拒绝），不能提权。pid 取恢复方的 pid。
- vDSO/vvar 等 `VM_PFNMAP` 窗口不入镜像，恢复时重映射当前内核的 vDSO。
- 镜像带 `arch` 与版本号，跨架构/跨布局版本拒绝（`-ENOEXEC`），绝不部分套用。

### 验收

```sh
make smoke-kexec-svc-checkpoint   # rv64：checkpoint→kexec→restore，计数器连续性断言
make test-ckpt-image              # Rust 校验与帧装配的宿主机单元测试
```

smoke 断言：恢复前后 tick 计数器单调连续（`tools/qemu/ckpt_svc_verify.py`）、恢复方 pid 不同、两次启动均 `[SMP] 2/2`。跨架构上寄存器经语义化 `proc_debug_regs_t` 交换，镜像不可跨架构移植。
