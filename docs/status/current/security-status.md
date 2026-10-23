# SECURITY 候选现状

核实日期：2026-10-23。当前分支 `codex/security-release`。平台为 QEMU RISC-V64
TCG、1 GiB、`ABI=both BRINGUP=0` dev，功能组合在 1 CPU 和 SMP2 验证；性能为
1 CPU。待处理事项只见 [SECURITY 台账](../gaps/security.md)，接口、设计边界、
默认值决策与可复制命令见 [安全候选验收](../../security/release-acceptance.md)。

## 当前行为

- Landlock create 检查 flags、size、用户内存、非零 handled 与零扩展；仅接受实际
  实现的 READ_FILE/WRITE_FILE。未知限制报错，不返回忽略请求字段的成功结果。
- Landlock 按解析目标路径检查普通 open、openat2 与 proc fd 重开；创建时在再次
  解析并 pin 的父目录上复核，拒绝发生在 create/truncate/backend open 前。
  fork/exec 与关闭 ruleset fd 后保持限制；首次安装冻结策略，多个任务可复用冻结
  fd。策略修改/读取由叶锁串行化，引用计数独立原子维护，超长前缀拒绝而非截断。
- envelope 的原 fd、dup、SCM_RIGHTS 接收引用在 revoke 返回后的下一次操作均得到
  EACCES。拒绝 read 不改输出缓冲，拒绝 write 不改文件；close 仍释放引用，退出后
  `n_shadows=0`。同 uid 非 owner、跨 uid 无 cap 不能撤销；owner 可撤销。fork/exec
  保留 envelope，操作/字节配额拒绝均无读写副作用。
- Native MMIO 按完整页跨度授权，拒绝零长度、回绕、执行权限、非 owner 和越界请求；
  输出失败撤销映射。驱动逐页映射部分失败撤销 PTE、清理失败页空路径，完成 TLB
  事务、释放临时 VMA；全部成功才发布。设备数据帧不由页表回滚释放。
- RV64 MMU 信号 restorer 由 VMO 持有 RX 映射、RX 权限上限；不再请求可写栈执行。
  fork、失败、退出复用 MM/VMO 生命周期；分配失败不降低 W^X 策略。
- 用户 W^X 默认保留 `off` 兼容 V8/JIT；候选显式 `a20.wx=deny`，未扩大翻译器
  豁免。内核 W^X、ASLR、栈保护、UBSan 保持开启。seccomp/Landlock/envelope
  由进程主动安装，IOMMU 依赖实际设备；默认启动不等于该安全组合。

## 当前验收结果

| 检查 | 结果 | 实际断言 |
| --- | --- | --- |
| `make check-honesty-policy check-smoke-cases` | PASS | fail-closed、lwIP 接线、185 个 case 可达且无孤儿 |
| syscall coverage 生成校验 | PASS | 声明与表格一致，Landlock 保持 partial |
| Landlock 宿主负控 / Rust 状态测试 | PASS | 创建参数拒绝零分配、移除校验负控失败；权限并集、分量边界、冻结复用、引用释放、长前缀拒绝 |
| Landlock guest | PASS | no-cap/no-new-privs、fd close、fork/exec、冻结共享；普通/别名/openat2/proc fd 拒绝无截断，创建拒绝无新文件，允许创建/读写正常 |
| envelope 撤销、继承、配额 | PASS | 正/负控制及既有 `envelope_smoke` 全部场景 |
| Native handle | PASS | rights 收窄、transfer、事件、temporal 操作耗尽/自动关闭、meta/xattr |
| Native VM/MMIO | PASS | VMAR 越界/权限上限/失败回滚；MMIO 非 owner、范围、执行权限、连续三次输出失败无 VA 泄漏；正常访问与释放 |
| MMIO 部分失败宿主故障注入 | PASS | 生产 mapper/unmap，跨两级页表边界分配失败；已映射前缀和空表回收、邻接映射不变、VMA 释放、TLB 配对；移除回滚/空表清理的负控失败 |
| PT 既有宿主回归 | PASS | sparse/dense/swap 空表检测及 MCS CPU 迁移负控 |
| credential | PASS | capset 拒绝矩阵、uid/gid/FSID 与 fork 隔离、并发 worker |
| `make smoke-wx-aslr` | PASS | RWX mmap/mprotect 拒绝、RW/RX 允许、exec ASLR；shell 返回后执行显式 poweroff |
| seccomp / sandbox | PASS | no_new_privs fork/exec/setid，pointer/errno/inherit/stack/scratch/mode/SIGSYS，dumpable/ptrace |
| IOMMU 两设备 | PASS | dev0 越界 DMA 阻断，dev1 继续，两域释放，关机 audit errors=0 |
| 安全候选 1 CPU / SMP2 | PASS | 上述 workload 全部执行；SMP2 实际 2/2 CPU 在线；无内核异常、audit errors=0、QEMU 自然 rc=0 |
| 普通 shell boot（WX off） | PASS | 外部 echo、显式 poweroff、零审计错误与自然 rc=0；仅为普通 boot 对照 |
| 安全开启性能 | PASS（测量） | 同输入三段采样；pread 中位数 +67.4%、open/close +13.3%；默认兼容策略与边界见参考页 |
| 其他 CPU/ABI、实板、DMA 吞吐、guest 全局 OOM | 未测 | 不从本矩阵外推 |
| 网络/scoping、inode 锚定与目录拓扑限制等完整 Linux Landlock 语义 | N/A | 路径子集的设计边界，未知 handled 请求报错，不计作隔离覆盖 |

SECURITY-01..04 已完成并从本领域台账删除；关联的 ABI-SECURITY-02/03、
FS-SECURITY-03、DRIVER-SECURITY-03 已随实现修复关闭。此结论仅关账本领域，
其他领域台账仍由各 owner 负责，**不代表整个 OS 1.0 release done**。
