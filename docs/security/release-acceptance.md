# 安全候选验收与使用

当前行为与逐项结果见 [SECURITY 现状](../status/current/security-status.md)，待处理事项
见 [SECURITY 台账](../status/gaps/security.md)。本页描述接口、运行条件和可复制命令；
不保存原始日志、历史提交标识或构建输入清单。

## 支持的权限边界

Linux capability 与 Native rights 是不同授权域。`cred_state_test` 覆盖 capset 的
permitted/effective/inheritable 规则、拒绝无部分应用和 uid/gid 隔离。Native 的
`native-handle-rv` 覆盖 rights 收窄、字节 transfer、temporal 操作预算和自动关闭。
Linux envelope 的 SCM_RIGHTS 测试是描述符授权传递，不把 Native 的字节 transfer
误称为句柄转移。

`security_release_test` 另以停止/恢复握手建立 revoke 顺序：原 fd、dup 与 SCM_RIGHTS
接收 fd 都先成功读；监督者等待 worker 停止后完成 revoke，再恢复 worker。后续
pread/pwrite 必须 EACCES，输出缓冲与文件内容不变，close 成功；worker 退出后
stats 必须显示 expired=1、n_shadows=0。该契约是下一次受控操作拒绝，不承诺取消
已经进入 I/O 后端的操作。另有同 uid 非 owner、跨 uid 无 cap 的拒绝，以及 owner
成功撤销的正控制；fork/exec 继承和两种配额耗尽分别检查。

Landlock create 的最小输入是 8 字节 `handled_access_fs`。flags 必须为零；size
须在 8 字节到一页之间，扩展区必须全零。错误分别为：

| 请求 | errno |
| --- | --- |
| 非零 flags 或 size < 8 | EINVAL |
| size > 一页或非零扩展字段 | E2BIG |
| 不可读用户内存 | EFAULT |
| handled=0 | ENOMSG |
| 含 READ_FILE/WRITE_FILE 之外的 handled 位 | EOPNOTSUPP |

校验在 ruleset 分配前完成。零扩展可接受；网络、scoping、执行、目录修改与 ioctl
限制不被静默丢弃。未提供标准 Landlock ABI 版本查询，也不宣称完整 Linux Landlock
ABI v1。当前字符串路径规则的 fork/symlink 验收结果见现状页，不能据一次成功的
`restrict_self` 断言整个进程已经隔离。

## 配置和命令

在本包 worktree 中运行，使用仓库公共 build lease；linked worktree 的 `.git` 是文件，
所以先从 common dir 定位脚本：

```sh
common=$(git rev-parse --path-format=absolute --git-common-dir)
runner="$common/parallel-rewrite/run-build.sh"
"$runner" build-2 python3 tools/tests/test_landlock_create_host.py
make check-honesty-policy check-smoke-cases
"$runner" build-2 make smoke-security-release
"$runner" build-2 make smoke-wx-aslr
"$runner" build-2 python3 tools/security_release.py boot
"$runner" build-2 python3 tools/security_release.py perf-off
"$runner" build-2 python3 tools/security_release.py perf-on
```

退出 75 表示共享槽忙、本次没有开始，不是 PASS/FAIL；槽释放后重新运行。脚本复用
现有 `tools/smoke.py` 的 QEMU lease、超时与断言。所有产物留在被忽略的构建目录。
新增 make target 只接线本包唯一 case，未修改 GLOBAL 默认、通用构建规则或 CLI。

安全候选选择 `a20.wx=deny`，挂载 RISC-V IOMMU 和两个 EDU 设备；内核仍使用 dev
配置的栈保护、内核 W^X、ASLR、UBSan。candidate 以 `exec` 把 shell 替换为测试监督者，
让每个独立 workload 都能执行到并给出结果。它没有证明 shell 的 SIGCHLD 路径可用：
原有 `smoke-wx-aslr` 仍独立运行并保留其失败。监督者退出后 init 正常关机，必须看到
POWER_OFF、audit errors=0 和 QEMU rc=0；功能 FAIL 即使 rc=0 也使总用例失败。

普通 boot 对照使用 WX off，检查 shell、外部 echo 与显式 poweroff。它通过只证明
普通 boot，不能用于替代安全开启验收。用户 W^X 默认仍为 off；翻译器豁免仍按原任务
标记生效。本包没有扩大豁免，也没有修改 MM/PROC 默认值。

## 安全开启时的代价

2026-10-23，在本包已提交的代码上实跑。固定 QEMU RISC-V64 TCG、默认 CPU、1 CPU、
1 GiB、同一 `ABI=both BRINGUP=0` dev 内核和同一测试程序。每种配置同一进程采三段：
每段 20,000 次 pread(64)、2,000 次 open/close，输入都是同一 64 字节 `x` 文件。
每次读都校验长度和内容，每次 open/close 都校验成功；一段读取 1,280,000 字节。
计时包含循环与内容校验，两组完全相同，不包含规则安装/驱动启动时间。

- off：WX off、不安装 seccomp/Landlock/envelope、不挂 IOMMU/EDU。
- on：WX deny，安装拒绝 getppid 的 seccomp、允许测试目录的 READ_FILE Landlock
  和有操作/字节预算的 envelope；先实际断言 getppid、目录外 open、socket 和 RWX
  mmap 分别被拒绝。IOMMU/EDU 运行越界阻断、另一域继续与释放正/负控制。
- 内核 W^X、ASLR、栈保护和 UBSan 在两组都保持开启；这些不可由该实验关闭的机制
  不属于所报差值。

| 操作 | off 中位数（范围）ns/op | on 中位数（范围）ns/op | 中位数差异 |
| --- | --- | --- | --- |
| pread(64) | 12,618（12,530–12,866） | 20,806（20,748–20,833） | +64.9% |
| open/close 对 | 97,767（97,075–97,831） | 121,492（120,513–122,357） | +24.3% |

两组 workload 均 PASS、无内核异常、审计零错误且自然 rc=0。on 表示机制已配置并且上述
控制真实生效，不认证它们的所有旁路。IOMMU 在该内核中真实启用，但本计时负载不发 DMA，
因此不报告 DMA 吞吐或 IOMMU 独立开销。三段来自各自一个 guest，不是三个独立冷启动；
TCG 与同机其他负载会影响绝对值，这组数据不能推导实板/桌面/服务器的总体代价。

判断：**不接受据这两个热点结果直接把完整安全组合设为通用默认值**。读路径的相对增加
明显；安全候选同时保留功能 FAIL，默认值决策不能只看性能。已明确选择隔离的受控进程
可以评估约 8.2 微秒/次 pread、23.7 微秒/次 open/close 的本机 TCG 增量，但这里没有
整机性能预算或实板依据，故不作通用性能合格声明。SECURITY-04 的测量与接受性判断已
完成；安全默认值与完整隔离由现存 SECURITY-02/03 继续约束。
