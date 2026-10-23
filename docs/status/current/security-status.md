# SECURITY 候选现状

核实日期：2026-10-23。平台为 QEMU RISC-V64 TCG、1 CPU、1 GiB、`ABI=both`、
`BRINGUP=0`，当前分支 `codex/security-release`。本页记录当前候选的实际行为；
待处理事项只见 [SECURITY 台账](../gaps/security.md)。接口、命令与代价见
[安全候选验收](../../security/release-acceptance.md)。

## 当前行为

- Linux Landlock create 在分配 ruleset 前检查 flags、size、用户内存、非零 handled
  和扩展区。只接受实际有 open 检查的 READ_FILE/WRITE_FILE 位；其他限制请求报错，
  不再返回一个忽略请求字段的成功结果。
- envelope 的原 fd、dup、SCM_RIGHTS 接收引用在 revoke 返回后的下一次操作均得到
  EACCES。拒绝 read 不改输出缓冲，拒绝 write 不改文件；close 仍可释放引用，退出后
  `n_shadows=0`。同 uid 非 owner 与跨 uid 无 capability 的调用者不能撤销，owner
  可撤销；fork 与 exec 保留 envelope。操作/字节配额耗尽的拒绝同样无读写副作用。
- Native MMIO 按完整页跨度授权，拒绝零长度、回绕和未支持的执行权限；输出地址拷贝失败
  时撤销映射。guest 用只读参数页连续触发三次失败，随后验证首个空闲地址仍可分配；
  非 owner、越界拒绝、正常映射与释放均通过。
- Native handle 使用现有 rights、temporal 操作预算与自动关闭实现，当前用例通过。
  Linux envelope 与 Native handle 的空间、继承和授权语义分别验证，不声明二者等价。
- 用户 W^X 默认 `off`，安全候选显式选择 `a20.wx=deny`。内核 W^X、ASLR、栈保护
  保持开启；seccomp/Landlock/envelope 为进程主动安装，IOMMU 需要实际设备。

## 当前验收结果

| 检查 | 结果 | 实际断言 |
| --- | --- | --- |
| `make check-honesty-policy` | PASS | fail-closed 与 lwIP 接线检查 |
| Landlock create 宿主负控 | PASS | 短参数、flags、未知限制、非零扩展拒绝且零分配；移除 flags 检查的负控会失败 |
| Landlock no-cap、关闭 fd、直接 exec | PASS | 无 no_new_privs 且无 CAP_SYS_ADMIN 时拒绝；拒绝不施加规则；安装后允许/拒绝路径和 exec 保持有效 |
| Landlock fork | FAIL | 父进程拒绝的文件，fork 子进程能打开 |
| Landlock symlink | FAIL | 允许目录内的符号链接能读到被拒绝的目标 |
| envelope 撤销、继承、配额 | PASS | 新增逐项正/负控制，加既有 `envelope_smoke` 全部场景 |
| Native handle | PASS | rights 收窄、transfer、事件、temporal 操作耗尽/自动关闭、meta/xattr |
| Native VM/MMIO | PASS | VMAR 越界/权限上限/失败回滚；MMIO 非 owner、范围、执行权限与输出失败回滚；正常访问与释放 |
| credential | PASS | `cred_state_test` 的 capset 拒绝矩阵、uid/gid/FSID 与 fork 隔离 |
| W^X/ASLR workload | PASS | RWX mmap/mprotect 拒绝、RW/RX 允许、exec 随机布局 |
| `make smoke-wx-aslr` 总用例 | FAIL | workload PASS，但 shell 的信号 trampoline 请求 RWX 后退出，显式 poweroff 未执行 |
| seccomp | FAIL（组合） | 基本 ERRNO/允许与 fork 控制通过；`sandbox_test` 停在 SIGSYS/TRAP；其后的 stack/setid 检查未测 |
| IOMMU 两设备 | PASS | dev0 越界 DMA 被阻断、dev1 继续、两域释放；关机 audit errors=0 |
| 安全开启性能 | PASS（测量） | 两种配置同输入，见参考页；不认证整个 OS 的性能 |
| 普通 shell boot（WX off） | PASS | 外部 echo、显式 poweroff、审计零错误、QEMU 自然 rc=0 |
| 安全候选总用例 | FAIL | 上述 Landlock 两项和 SIGSYS 失败均保留，QEMU 自然 rc=0 不会覆盖功能失败 |
| 其他 CPU/ABI、实板、DMA 吞吐与驱动页表分配故障注入 | 未测 | 本次结论仅覆盖表头平台和 workload |
| 未提供的网络/scoping 等 Landlock 限制 | N/A | 创建入口报错，不计作隔离覆盖 |

SECURITY-01、SECURITY-04 已删除：前者的现有状态机无需新增重写，当前针对性控制验证了
表列支持路径；后者已有真实同输入数据与默认值判断。SECURITY-02/03 仍保留。
跨模块缺陷已分别登记为 [ABI-SECURITY-02/03](../gaps/abi-ipc.md) 与
[FS-SECURITY-03](../gaps/fs.md)；驱动逐页映射失败回滚的源码问题另见
[DRIVER-SECURITY-03](../gaps/drivers.md)。当前安全候选不能关账，也不代表 OS 1.0 完成。
