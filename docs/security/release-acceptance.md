# 安全候选验收与使用

当前行为与逐项结果见 [SECURITY 现状](../status/current/security-status.md)，待处理事项见 [SECURITY 台账](../status/gaps/security.md)。本页描述接口、运行条件和可复制命令；不保存原始日志、历史提交标识或构建输入清单。

## 支持的权限边界

Linux capability 与 Native rights 是不同授权域。`cred_state_test` 覆盖 capset 的 permitted/effective/inheritable 规则、拒绝无部分应用和 uid/gid 隔离。Native 的`native-handle-rv` 覆盖 rights 收窄、字节 transfer、temporal 操作预算和自动关闭。 Linux envelope 的 SCM_RIGHTS 测试是描述符授权传递，不把 Native 的字节 transfer 误称为句柄转移。

`security_release_test` 另以停止/恢复握手建立 revoke 顺序：原 fd、dup 与 SCM_RIGHTS 接收 fd 都先成功读；监督者等待 worker 停止后完成 revoke，再恢复 worker。后续 pread/pwrite 必须 EACCES，输出缓冲与文件内容不变，close 成功；worker 退出后 stats 必须显示 expired=1、n_shadows=0。该契约是下一次受控操作拒绝，不承诺取消已经进入 I/O 后端的操作。另有同 uid 非 owner、跨 uid 无 cap 的拒绝，以及 owner 成功撤销的正控制；fork/exec 继承和两种配额耗尽分别检查。

Landlock create 的最小输入是 8 字节 `handled_access_fs`。flags 必须为零；size 须在 8 字节到一页之间，扩展区必须全零。错误分别为：

| 请求 | errno |
| --- | --- |
| 非零 flags 或 size < 8 | EINVAL |
| size > 一页或非零扩展字段 | E2BIG |
| 不可读用户内存 | EFAULT |
| handled=0 | ENOMSG |
| 含 READ_FILE/WRITE_FILE 之外的 handled 位 | EOPNOTSUPP |

校验在 ruleset 分配前完成。零扩展可接受；网络、scoping、执行、目录修改与 ioctl 限制不被静默丢弃。未提供标准 Landlock ABI 版本查询，也不宣称完整 Linux Landlock ABI v1。

每任务至多安装一个 ruleset；fork 在子任务发布前取得引用，exec 保留限制，关闭 ruleset fd 不解除限制。首次安装冻结规则，其他任务可重复安装同一冻结 fd，后续 add_rule 报 EINVAL。策略最多 64 个、每个 255 字节的绝对路径前缀；过长报 ENAMETOOLONG，不截断成更宽的授权。匹配按目录分量边界检查，多个命中规则的权限取并集。C 叶锁串行化策略修改/冻结/读取，引用计数独立原子维护；不在锁内执行 VFS、分配或 usercopy。

规则目录和 open 目标都解析为 canonical path。普通 open 与 openat2 在同一次遍历返回的 vnode 上执行检查，再进入 create/truncate/backend open；proc fd 重开也检查保留的目标路径。测试既允许目录内读写，又拒绝文件/目录符号链接、相对别名、 openat2 与 proc fd 的外部目标，验证拒绝不截断、不创建。安装前已有描述符按原契约保留权限；该路径子集不是 Linux 的 inode 锚定 LSM，不约束 rename/link/mount 等未支持的目录拓扑修改，也不撤销已有文件描述符。因此仅靠 READ_FILE/WRITE_FILE 规则不足以承载允许不可信进程修改目录拓扑的完整 Linux Landlock 沙箱；应用需要同时限制这些未授权的系统调用。设计边界不计作已覆盖的隔离功能。

RV64 MMU 的信号 restorer 通过匿名 VMO 的 RX 映射持有，权限上限也是 RX，不请求可写栈执行，不增加 W^X 豁免。VMO/VMA 生命周期负责 fork、失败与退出清理；分配失败保留原有终止行为，不降级安全策略。

## Native MMIO 边界

`device_map_mmio` 只支持读/写保护位。长度必须非零，物理基址页对齐，向上取整及范围加法不可回绕。授权检查使用向上取整后的完整页跨度；返回错误时不改用户 `out_addr`。如果映射已经建立而输出拷贝失败，ABI 入口通过 MM unmap 撤销新映射，返回 FAULT。

候选调用 `native-mm-rv --security-device`：EDU/IOMMU 必须存在，检查未 claim 的任务不能 map/release；claim 后的零长度、回绕、BAR 外范围和执行权限请求被拒绝；只读输出参数连续失败三次后，用真实 VM 分配器确认空闲地址没有被泄漏映射占用。最后正常映射、读取 EDU 标识、release 并确认旧 VA 已被解除。普通 `native-mm-rv` 不要求 PCI 设备。该控制也完整运行原有 VMAR 越界、权限上限和 remap 回滚用例。

宿主夹具直接运行生产 ABI 函数，分别移除完整页跨度授权、输出失败回滚作为负控。驱动后端的 `pt_map` 部分失败会先清理失败页的空路径，再撤销已映射前缀；全部成功才发布 VMA。生产 mapper 和 `pt_unmap` 的宿主夹具在两级页表边界注入分配失败，检查没有剩余 PTE/临时 VMA/空页表、邻接映射不变及 TLB 事务配对；分别移除回滚与空表清理的负控均失败。故障注入替换分配/硬件边界，不宣称 guest 全局 OOM。锁序是 tlb mutex → mm spinlock → 页表节点锁；锁外结束 TLB 事务和 deferred 清理，不释放设备数据帧。

## 配置和命令

在仓库工作目录中运行，使用公共 build lease；linked worktree 的 `.git` 是文件，所以先从 common dir 定位脚本：

```sh
common=$(git rev-parse --path-format=absolute --git-common-dir)
runner="$common/parallel-rewrite/run-build.sh"
"$runner" build-2 python3 tools/tests/test_landlock_create_host.py
"$runner" build-2 python3 tools/tests/test_security_mmio_host.py
"$runner" build-2 python3 tools/tests/test_udriver_mmio_rollback_host.py
"$runner" build-2 python3 tools/tests/test_pt_table_empty.py
"$runner" build-2 python3 tools/tests/test_pt_mcs_preempt_window.py
"$runner" build-2 sh -c 'rustc --edition=2021 --test kernel/ipc/landlock.rs -o /tmp/a20-landlock-tests && /tmp/a20-landlock-tests'
make check-honesty-policy check-smoke-cases
"$runner" build-2 make smoke-security-release
"$runner" build-2 python3 tools/smoke.py smoke-security-release --nr-cpus 2
"$runner" build-2 make smoke-wx-aslr
"$runner" build-2 python3 tools/release/security_release.py boot
"$runner" build-2 python3 tools/release/security_release.py perf-off
"$runner" build-2 python3 tools/release/security_release.py perf-on
```

退出 75 表示共享槽忙、本次没有开始，不是 PASS/FAIL；槽释放后重新运行。脚本复用现有 `tools/smoke.py` 的 QEMU lease、超时与断言。所有产物留在被忽略的构建目录。新增 make target 只接线本包唯一 case，未修改 GLOBAL 默认、通用构建规则或 CLI。

安全候选选择 `a20.wx=deny`，挂载 RISC-V IOMMU 和两个 EDU 设备；内核仍使用 dev 配置的栈保护、内核 W^X、ASLR、UBSan。candidate 以 `exec` 把 shell 替换为测试监督者，让每个独立 workload 都能执行到并给出结果。原有 `smoke-wx-aslr` 独立覆盖 shell 的 SIGCHLD 与显式 poweroff；候选的 root 监督者不能替代这个控制。监督者退出后 init 正常关机，必须看到 POWER_OFF、audit errors=0 和 QEMU rc=0；功能 FAIL 即使 rc=0 也使总用例失败。

普通 boot 对照使用 WX off，检查 shell、外部 echo 与显式 poweroff。它通过只证明普通 boot，不能用于替代安全开启验收。用户 W^X 默认仍为 off；翻译器豁免仍按原任务标记生效。本包没有扩大豁免，也没有修改 MM/PROC 默认值。

## 安全开启时的代价

2026-10-23，在本包已提交的代码上实跑。固定 QEMU RISC-V64 TCG、默认 CPU、1 CPU、 1 GiB、同一 `ABI=both BRINGUP=0` dev 内核和同一测试程序。每种配置同一进程采三段：每段 20,000 次 pread(64)、2,000 次 open/close，输入都是同一 64 字节 `x` 文件。每次读都校验长度和内容，每次 open/close 都校验成功；一段读取 1,280,000 字节。计时包含循环与内容校验，两组完全相同，不包含规则安装/驱动启动时间。

- off：WX off、不安装 seccomp/Landlock/envelope、不挂 IOMMU/EDU。
- on：WX deny，安装拒绝 getppid 的 seccomp、允许测试目录的 READ_FILE Landlock 和有操作/字节预算的 envelope；先实际断言 getppid、目录外 open、socket 和 RWX mmap 分别被拒绝。IOMMU/EDU 运行越界阻断、另一域继续与释放正/负控制。
- 内核 W^X、ASLR、栈保护和 UBSan 在两组都保持开启；这些不可由该实验关闭的机制不属于所报差值。

| 操作 | off 中位数（范围）ns/op | on 中位数（范围）ns/op | 中位数差异 |
| --- | --- | --- | --- |
| pread(64) | 12,481（12,454–12,975） | 20,899（20,616–31,079） | +67.4% |
| open/close 对 | 105,000（101,498–107,889） | 118,962（118,424–122,608） | +13.3% |

两组 workload 均 PASS、无内核异常、审计零错误且自然 rc=0。on 表示机制已配置并且上述控制真实生效，不认证它们的所有旁路。IOMMU 在该内核中真实启用，但本计时负载不发 DMA，因此不报告 DMA 吞吐或 IOMMU 独立开销。三段来自各自一个 guest，不是三个独立冷启动； TCG 与同机其他负载会影响绝对值，这组数据不能推导实板/桌面/服务器的总体代价。

默认值决策：**保留用户 W^X off 的兼容默认值**，需要隔离的进程显式选择 WX deny 并主动安装 seccomp/Landlock/envelope。IOMMU 需要硬件设备与驱动实际配置；不能将“默认启动”称为该安全组合。开启组合的功能矩阵已通过，兼容默认值的原因仍包括 V8/JIT 写入执行页的既有需求与可观的读路径增量。本包没有扩大翻译器豁免，也未修改 GLOBAL 默认或通用构建接线。

本机 TCG 中位数增量约为 8.4 微秒/次 pread、14.0 微秒/次 open/close；on 首段读达到 31,079 ns/op，范围一并报告，不删除较慢段。受控隔离 workload 可以按此选择代价，但没有整机预算或实板数据，故不作通用性能合格声明。SECURITY-02 的安全开启验收与默认值决策、SECURITY-04 的同输入测量均已完成；其他领域仍由对应台账约束。
