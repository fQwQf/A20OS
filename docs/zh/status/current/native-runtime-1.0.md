# Native 动态运行时与 exec 状态

> 未完成事项见 [1.0 缺口台账](../gaps/README.md)；本页描述当前实现与验证结果。验证平台：QEMU `virt`，RISC-V64、AArch64；ABI=both。

Native 1.1.0 运行时接口、SDK/适配层分工与迁移见 [扩展规范](../../native-abi/10-runtime-extensions.md)；相关验证记录参见 [mlibc 加固验收记录](../history/mlibc-native-hardening-2026-10-23.md)。

本页覆盖 package 的四个验收面：真实 mlibc 动态链接、exec 继承、kexec staging/handoff 边界、NOMMU 平台矩阵。旧的 fakeld 探针仍保留在 `smoke-native-dynlink`，但动态链接的验收不再以它为依据。

Native 信号遵循[检查点式通知设计边界](../../native-abi/checkpoint-signals.md)：普通通知不在任意用户指令位置执行处理器，内核抢占与强制终止独立生效。完整异步处理器不属于 1.0 必须项；本页的动态链接、线程和 exec 验收不改变该边界。

## 1. 真实动态 Native 程序

`user/external/mlibc/sysdeps/a20` 现在同时构建静态与共享制品：

- `rtld.cpp` 是 ld.so 专用的最小 sysdep 集合（fd 表、Open/Read/Seek/Close、 VmMap/VmUnmap/VmProtect、AnonAllocate/AnonFree、Stat、Futex*、TcbSet、 LibcLog/LibcPanic/Write）。它不引用 libc 的 malloc/calloc；ld.so 在 libc.so 存在之前就能完成自举。
- `sysdeps/a20/rtld-entry.S`（RV64/AArch64 分支）在 `relocateSelf` 之前把内核传入的`a20_start_info_t` 保存到 `__a20_rt_start_info`，并在跳转到应用入口时把 a0/x0 恢复成同一个 start_info；解释器与应用看到同一个句柄表。
- A20 的 `mlibc` 配置启用 `MLIBC_MAP_DSO_SEGMENTS`：每个 PT_LOAD 按最终权限直接从文件映射。native ABI 的 mprotect 不会把 VMA 权限提升到创建时的 cap 之外，因此不能走“先 RW 再 mprotect(RX)”的通用路径。
- `user/mlibc/a20-mlibc-dyn.ld` 是动态 PIE 脚本：PT_INTERP、PT_PHDR、PT_DYNAMIC、 PT_TLS 与 `0x6a20a200` Native 标记同时存在。

构建与运行：

```sh
make mlibc-shared-sysroot     # ld.so + libc.so（linux-gnu 驱动只用于 -shared 链接）
make mlibc-dynamic            # 动态程序 + dlopen 插件
make smoke-mlibc-dynamic      # QEMU guest：真实 ld.so/libc.so
```

`smoke-mlibc-dynamic` 在清空 `LD_LIBRARY_PATH` 后启动 `/bin/mlibc-dynamic-rv`，因此`libc.so` 只能由可执行文件的 `RUNPATH=$ORIGIN/lib` 找到。负载断言：

- 通过共享 libc 调用 `snprintf`/`strlen`/`qsort`；
- 可执行文件与 dlopen 插件中的 `_Thread_local` 在 4 个 pthread 中各自独立；
- `dlopen`/`dlsym` 真实 DSO 并调用；
- `atexit` 在正常退出路径执行；
- `execve` 重新执行同一镜像，第二实例打印 marker 后正常退出。

### 权限边界

private 文件映射的写权限仍受 handle rights 上限约束（`docs/zh/native-abi/04-memory.md` §4.2）。因此 rtld 以 `O_RDWR` 打开 DSO；在只读介质上加载可写段目前会失败。这是 native ABI 的 cap 语义，不是 rtld 的临时绕过；若要支持只读 rootfs，需要在 MM 层区分 private COW 写权限与文件写权限。

## 2. exec 继承矩阵

`/bin/exec_switch_probe`（`smoke-elf-exec-rust`）在 exec 前建立多线程、fd 与信号状态， exec 后逐项检查：

| 继承项 | 行为 | 用例 |
|--------|------|------|
| 非 CLOEXEC fd | 保留 | `kept_ok=1` |
| FD_CLOEXEC fd | 关闭 | `closed_ok=1` |
| 兄弟线程 | exec 事务终止，不再存活 | `alive=1` |
| TLS / TP | 新映像重新建立 | `tls_ok=1` |
| 凭据 | 无 setuid 时不变 | `creds_ok=1` |
| 捕获的信号处置 | 复位为 SIG_DFL | `hup_default=1` |
| SIG_IGN 处置 | 跨 exec 保留 | `pipe_ignored=1` |
| 信号阻塞掩码 | 清空 | `mask_cleared=1` |
| 符号链接路径 | exec 跟随链接并到达同一映像 | 探针经 `/tmp/exec-switch-link` exec |
| `$ORIGIN` | 动态程序的 RUNPATH 以 AT_EXECFN 为基准解析 | `smoke-mlibc-dynamic`（清空 LD_LIBRARY_PATH） |

内核在 conventional auxv 中发布 `AT_EXECFN`（`kernel/mm/elf_core.rs` 的栈规划器，路径来自 `elf_load_info_t.path`）；从内存缓冲区加载的 init 没有路径，因此省略该条目而不是发布空指针。`signal_exec_reset()` 在复位处置的同时清空 `sig_blocked`。

复跑入口：

```sh
make smoke-elf-exec-rust   # exec 继承矩阵 + 负路径 + auxv
make smoke-mlibc-dynamic   # 动态 $ORIGIN / dlopen / TLS
```

## 3. kexec：事务暂存与真实 handoff

`kexec_load_segments()` / `kexec_load_file()` 完成 ELF 物理段校验、分页拷贝、替换与回滚；`smoke-elf-kexec-rust` 覆盖畸形段、失败替换保留旧镜像和 Linux unload；Native 状态查询由人格内的 handoff 用例覆盖。真实执行由 `smoke-kexec-handoff`（Linux 文件加载）和`smoke-kexec-handoff-native-smp2`（Native 内存加载）分别验收：同一 QEMU 进程完成 SMP 停核、VFS 关闭、设备复位、物理复制和第二次 init，并检查持久标记。

当前 handoff 后端仅承诺 `riscv64 + qemu-virt-riscv64 + MMU`。其他架构、NOMMU 和物理板型仍返回不支持；最终跳转后没有 A/B 自动回滚。设计与操作见[内核热切换](../../kernel/kexec-hot-update.md)。

## 4. NOMMU / 平台矩阵

`make smoke-nommu-exec` 使用 `ARCH=riscv64 ABI=both NOMMU=1` 启动 QEMU，断言静态 ELF `/bin/echo` 被加载执行并自然关机。NOMMU 的 ELF 装载走 flat allocation ledger （`elf_discard_nommu_allocs` / `NOMMU_ALLOC_*`），不建立页表。

| 平台 | 结果 | 依据 |
|------|------|------|
| riscv64 NOMMU (QEMU virt) | ELF exec 已验证 | `smoke-nommu-exec` |
| aarch64 NOMMU | 待独立运行 | 保留实例 `qemu-aarch64-nommu`；独立于当前验证路径 |
| riscv32 NOMMU | 待独立运行 | 保留实例 `qemu-riscv32-nommu`；独立于当前验证路径 |
| arm32 NOMMU | 用户态运行时阻塞 | 与本页无关的 fork/reap 问题见 [gaps/core.md](../gaps/core.md) |
| x86_64 / loongarch64 NOMMU | N/A | `run-nommu-*` 明确标注 “not supported” |
| MCU profile | build-only | 只验证对象构建，不作为 guest 运行证据 |

2026-11-16 当前 RV64 `ABI=both NOMMU=1 NR_CPUS=2` 默认镜像已通过 `make -j4 smoke-nommu-exec`：连续运行两次外部 `/bin/echo`，父 shell 继续接受 `poweroff`，正常关机且 QEMU 退出 0。门禁拆分输入中的标记并匹配独立输出行，负控拒绝只含命令回显的日志；原 CORE-RUNTIME-05 已关闭。当前 init 使用 BusyBox 用户工具并 exec `sh`，此结果对应当前发布配置，不将历史 mksh 镜像或所有 NOMMU 架构计作覆盖。证据见 [本轮进度](v1.0-progress.md#本轮完成与验证2026-11-16)。
