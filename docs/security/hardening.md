# A20OS 安全加固状态

最后核实：2026-09（当前工作树）。验证记录均为历史记录，引用为当前
结论前须在当期提交上重新运行。

## 诚实性原则（fail-closed）

> 未实现的能力**必须报告「不存在」，绝不能伪造输出**。

这是 A20OS 对安全相关接口的第一原则。理由不是洁癖，而是失效模式的不对称：

一个**缺失**的能力，调用方会得到错误并走降级路径。
一个**返回成功的桩**，调用方无从察觉，会带着错误信念继续执行；
而且往往正是它最脆弱的地方在相信内核。

本轮修复的每一处都属于后者，且都属于「看起来实现了、实际没有」：

| 接口 | 修复前的行为 | 现在 |
|---|---|---|
| `AF_ALG` sha256 | `0xa5^i` 异或折叠的伪摘要 | `bind()` 返回 `-ENOENT` |
| `AF_ALG` cbc(aes) | 明文原样返回（恒等函数） | 同上；无 provider 时不产生任何数据 |
| `setsockopt(SO_BINDTODEVICE)` | 返回 0，选项被丢弃 | 返回 `-EOPNOTSUPP` |
| `getsockopt(TCP_CONGESTION)` | 报 `cubic`，实际只有 lwIP Reno | 报 `reno` |
| `getsockopt(TCP_INFO)` | 写 1 字节状态、其余清零 | 返回 `-EOPNOTSUPP` |
| `fsync()` | 只到设备易失写缓存，断电可丢 | 走到设备 flush（见下「存储持久性」） |
| `/proc/<pid>/io` | `rchar` 打印 CPU ticks | 真实 I/O 计数 |
| `/proc/loadavg` | 常量 `0.00 0.00 0.00 1/64 1` | 真实 1/5/15 分钟 EMA |
| `/proc/pressure` | 算出 avg60/300 后打印 `0.00` | 三档均值均真实输出 |
| `reboot()` | 无任何权限检查 | 要求 `CAP_SYS_BOOT` |
| `prlimit64(pid, ...)` | 丢弃 `pid`，只能操作自己 | 支持跨进程，受权限约束 |
| `setrlimit(未实现项)` | 返回 0 | 返回 `-EINVAL` |

`make check-honesty-policy` 以否定断言锁定上述性质：它检查这些反模式
不会重新出现，而不只是检查某个标记字符串存在。该门禁本身经过「故意引入
回归」的验证。改回旧行为后它会报错并非零退出。

一条推论适用于所有枚举型接口：未被显式处理的取值应各自返回
`-EOPNOTSUPP`/`-EINVAL`，**不要落进统一的 `return 0` 兜底**。否则下一个
新增的取值会自动被兜底吞掉，这正是 `SOL_SOCKET` 曾发生的事。

## 已落地


| 特性 | 实现 | 验证 |
|---|---|---|
| 用户指针校验 | `kernel/mm/mm.c` 的 `copy_from_user`/`copy_to_user` 统一入口：范围检查（`user_range_ok`，防回绕）+ 逐页 PTE_V/PTE_U/可写校验 + COW 折断；无 KERNEL_DS/set_fs 模式 | 全部 syscall 路径强制经过 |
| 监督模式访问防护 | x86_64 `x86_64_enable_smep_smap()`（CPUID leaf 7 门控 CR4.SMEP/SMAP，`trap_init` 每 CPU 调用）；aarch64 `aarch64_enable_pan()`（ID_AA64MMFR1_EL1.PAN 门控 SCTLR_EL1.PAN）；riscv64 用户 trap 入口显式 `csrc` 清除 SUM 位（保持 SUM=0，不只是"从未置位"）。内核不直接解引用用户 VA。Linux ABI 与 Native ABI 的用户指针访问统一经 `copy_from/to_user` 的直映射拷贝辅助（Native net 的 bind/connect/accept/getname 原先把裸用户指针交给核心层解引用，已改为内核缓冲暂存 + copy 进出），故无需 stac/clac 窗口 | `smoke-x86_64`/`smoke-aarch64`（默认 CPU 空操作路径）；`-cpu max` 完整引导到 mksh 无 fault（防护实际生效路径）；`smoke-riscv64`/`smoke-abi-linux` 验证 riscv64 无隐藏直接解引用 |
| 用户态 W^X | `kernel/mm/wx.c` 单一策略 `mm_wx_filter_prot()`，mmap/mprotect/ELF 装载全路径过滤；默认 deny（-EACCES），cmdline `a20.wx=strip|off` 可降级 | `user/cmds/stress/wx_aslr_test.c`（RWX 拒绝）；全量 195 个静态 musl 二进制扫描零 RWX PT_LOAD |
| 内核自身 W^X（KXAN） | `kernel/arch/{riscv64,x86_64,aarch64}/mm/kwx.c`：`arch_kernel_wx_finalize()` 在 `mm_init()` 后把引导大页旁路重建为 text=ROX/rodata=RO/data=RW+NX、直映射 NX；drvmod 模块 text=RX（`arch_kwx_module_protect`）；aarch64 置回 SCTLR_EL1.WXN | 启动自检（`mm_query_leaf` 四点断言，失败 panic）+ `[KXAN]` 日志行；smoke-riscv64/abi-linux/x86_64/aarch64 + smoke-smp-bringup（2 核）全 PASS。边界见 `docs/security/kernel-wx.md` |
| ASLR | `kernel/mm/aslr.c`：栈顶向下页对齐偏移（64 位 10 位熵，受 vdso_layout 约束）、mmap 基址 per-process 随机（64 位 20 位熵）、brk 起始随机偏移；PIE 基址原有 11 位熵。fork 继承布局（Linux 语义） | `wx_aslr_test`：两次 exec 栈/mmap 地址不同；`cat /proc/self/maps` 两次运行 `[stack]` 区间不同 |
| 内核栈 canary | `kernel/core/stack_protector.c`：`__stack_chk_guard`（熵池就绪后随机化）+ `__stack_chk_fail`；`-fstack-protector-strong` 默认开（`CONFIG_STACK_PROTECTOR`） | 全架构零警告构建；smoke 无 stack smashing |
| seccomp | `kernel/ipc/seccomp.c`：cBPF verifier + STRICT/FILTER 模式 + fork 继承，挂在 syscall 主路径。树内无任何服务安装 filter，机制可用但默认未施加 | `smoke-syscall-ext` |
| capabilities | 16 个 caps 子集（`kernel/include/proc/proc.h`），capset EPERM 矩阵在 `kernel/proc/cred.c`。缺 CAP_NET_ADMIN/CAP_BPF/CAP_SYSLOG | syscall 覆盖表 |
| `/proc/<pid>/pagemap` 访问门 | 逐条 PFN 记录曾对任意 uid 可读（`procfs_fread` 中唯一漏掉 `proc_task_may_access` 的 per-pid 读取器），可击穿用户态 ASLR 并暴露内核直映射布局 | 已加归属检查，跨 uid 读返回 `-EACCES` |
| 权限门：reboot/poweroff | `sys_reboot` 的每个出口都会终止机器，而 magic 是公开 ABI 常量而非密钥 | 要求 `CAP_SYS_BOOT` 或 `euid==0`，与 `sys_missing.c` 的 kexec 入口同一约定 |
| 资源限制 | `RLIMIT_STACK`/`CORE`/`NOFILE` 可设可强制；`RLIMIT_AS`（`proc_mmap` 前置检查）与 `RLIMIT_NPROC`（按 real uid 计数，`CAP_SYS_RESOURCE` 豁免）已实现并强制；`prlimit64` 现支持跨进程 | `smoke-proc-stress` |
| 诚实性门禁 | `make check-honesty-policy`：以否定断言锁定「未实现即报告不存在」，防止上述反模式回归 | 门禁自身经「故意引入回归」验证会失败并非零退出 |
| IOMMU/DMA 隔离 | RISC-V IOMMU per-device domain 动态 claim/map/unmap + fault queue 消费（fail-closed），`/proc/a20/iommu` 计数器 | `smoke-iommu-udriver-isolation`（授权内成功/窗口外 fault 被消费并阻断） |
| UBSan | 开发 profile 默认 `-fsanitize=undefined`（bringup/benchmark 关闭） | 全部 smoke |

## 存储持久性

`fsync()` 此前只保证「数据到达设备」，而设备的易失写缓存仍会在断电时丢失
内容：即向调用方承诺了它无法兑现的持久性。对服务器上的数据库而言这是
第一红线：被告知已提交的事务可能在断电后消失。

现状：

- `block_dev_t` 新增可选 `flush` 原语（`kernel/include/drivers/block/block_dev.h`）。
  指针为 NULL 明确表示「无持久性保证」，而非「尚未实现」。
- `bcache_sync_common()` 在写完数据之后、且在缓存锁之外调用它。
- 实现：virtio-blk 的 `VIRTIO_BLK_T_FLUSH`（按 virtio-blk 1.1 5.2.6 只挂
  header + status，不带数据描述符）、loop 转发到 backing file 的 fsync、
  AHCI 的 `FLUSH CACHE EXT`（0xE7，LBA=0xFFFFFFFF + count=0 表示全盘刷新）。
- **易复发点**：`fs/mount_setup.c` 的 `partition_block_dev` 与
  `class_block_dev` 两层包装原先只转发 read/write 而不转发 `flush`。由于
  所有挂载都位于分区之上，这会静默剥离整个文件树的持久性能力。现已修复；
  新增包装层时必须转发全部能力。

验证入口：`make smoke-fsync-durability`。该门禁断言 `block_flushes` 计数器
确实增长，而非仅检查 `fsync()` 返回 0。后者在修复前同样返回 0，所以
只检查返回值等于什么都没验证。

未覆盖：AHCI 路径仅完成编译验证（`ahci.c` 位于 `CONFIG_AHCI` 之后，树内
没有实例挂载 AHCI 控制器，补一个挂 ich9-ahci 的门禁是缺失的一环）；ext4
仍无可写 journal，详见 [roadmap/next-horizon.md](../roadmap/next-horizon.md)。

## 已评估、暂缓或进行中的项

### KASLR（暂缓，前置重构成本大）

内核链接地址固定（如 riscv64 `VIRT_BASE = 0xFFFFFFC080200000`）。
真 KASLR 的障碍不是代码重定位（`-mcmodel=medany` 的文本基本可搬），
而是直映射偏移 `PAGE_OFFSET` 是编译期常量并在全内核内联使用
（所有 `pa + PAGE_OFFSET` 形式的直映射访问、外设 MMIO 基址、页表
遍历宏）。可行路线：先把 PAGE_OFFSET 变成 per-boot 运行时变量
（所有引用点收编为 `phys_to_virt()` 类访问器），再在入口汇编按熵源
随机选取偏移并重建引导页表。这是一次横跨 arch/mm/drivers 的大型
重构，收益主要在远程内核信息泄漏威胁模型下；当前威胁模型（研究型
OS、无远程攻击面压力）下暂缓，先完成直映射访问器化作为前置。

### 内核栈 guard page（未做，替代缓解已部分就位）

内核栈是 kmalloc 对象（`kernel/proc/fork.c`），相邻对象间无隔离页。
在 slab 体系下加 guard page 需要分配器支持（对齐翻倍分配 + 底部页
取消映射），成本中等。已有的替代缓解：栈 canary（已落地）、slab
大块尾部哨兵（`kernel/mm/slab.c`）、trap 入口 sp 双界校验（取证硬化
的一部分）。guard page 仍是深度防御缺口，待内存损坏根因收口后评估。

### loongarch64 / ppc64le 的监督模式访问防护（硬件机制缺口）

LoongArch 无 SUM/PAN 等价 CSR 机制；ppc64le 的 KUAP（AMR/IAMR）
需要 key 管理体系，均未实现。两架构当前依赖直映射拷贝集中化 +
软件权限校验，与 SMAP 前的 x86 形态相同。

### user namespace（已实现，nsproxy 仍缺）

`kernel/proc/userns.c` 提供完整的 user namespace：`unshare(CLONE_NEWUSER)`
与 `clone(CLONE_NEWUSER)` 创建命名空间，`/proc/<pid>/{uid,gid}_map` 与
`setgroups` 由父命名空间写入，`setns(2)` / `listns(2)` 支持 user 类型，
`/proc/<pid>/ns/user` 已实现。门禁 `smoke-userns`。

`task_t::cred` 存**全局**（宿主）id，在 syscall 与 procfs 边界翻译，
与 Linux 的 `kuid_t`/`kgid_t` 同构；未映射的全局 id 呈现为
`USERNS_OVERFLOW_UID/GID`（65534），文件属主判定另用
`userns_kuid_mapped()` 区分"无表示"与"恰好是 65534"。

能力作用域集中在 `userns_capable()` 一处：自 `@ns` 沿 parent 链上溯，
只有正好落到调用者自己的命名空间时才按其 `cap_effective` 判定；越过
自己的层级即为拒绝。**没有**"创建者对父命名空间保有权限"这一条——
加上它就等于任何用户都能靠 `unshare -U` 拿到宿主 root。无特权的例外
只有一条，且必须由写入者本人发起：恰好一段、长度为一、映射自己的 id
（`unshare -U; echo $USER > /proc/self/uid_map`）。

capabilities 本身仍是 15 个子集（缺 CAP_NET_ADMIN/CAP_BPF/CAP_SYSLOG
等）；`nsproxy` 未实现，所以 `setns()` 一次只能切一种命名空间。

### user namespace 的引用模型

每个命名空间持有 parent 的一份引用（在 `g_userns_lock` 下取得），
`task_t::user_ns` 持一份，字段为 NULL 表示初始命名空间（静态钉住）。
释放只发生在真正的 task teardown：新拆出的 `fdtable_release_files()`
把"交出描述符表"和"task 消失"分开，因为 `fdtable_share()`（线程共享
表）原先走 `fdtable_close_all()`，会在一个活着的 task 上清空
mntns / pidns / userns 指针。

## 熵源说明

`kernel/core/random.c`（xoshiro128+）每 64 次提取混合一次熵源：
timer 节拍、内核地址、空闲帧计数、当前任务指针；x86_64 另有
RDRAND/RDSEED（`arch_hw_entropy_sample()`）。riscv64/aarch64 启动
早期熵质量有限，ASLR/guard 的不可预测性在真实硬件上依赖时钟噪声，
属常见启动期熵不足问题，暂无 virtio-rng 接入。
