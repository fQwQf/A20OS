# Paper 与代码仓库对齐审计

审计日期：2026-09-04  
审计提交：`ca6a26581f010c35ecf6273559b968bffe8274b0`（`main`）  
论文：`docs/paper/main.tex`  
结论：**部分对齐，但当前版本不宜按论文现有措辞宣称“完整、双 ABI、可复现地验证”。** 核心 Linux ABI 原型、主要自测场景和 canonical replay 可以运行；论文最重要的完整性、传递安全、实验归因及形式化对应关系仍有阻断级缺口。

## 1. 审计口径与验证结果

本审计逐项对照论文中的设计、实现、评估、形式化验证和可复现性主张，检查对应源码、生成器、测试验收条件与仓库内证据。严重级别定义如下：

- **阻断**：直接否定论文核心主张，或使实验结论不能由当前仓库证据支持；投稿前应修复实现或降级论文措辞。
- **高**：不会否定整个原型，但会明显改变安全边界、实验解释或可复现性。
- **中**：数字、接口或文档漂移，以及较窄但真实的实现缺口。

本次实际运行：

```text
CCACHE_DISABLE=1 make smoke-envelope smoke-envelope-pilot \
  smoke-envelope-bench smoke-envelope-corpus \
  NETDEV_USER='-netdev user,id=net'
```

四个 QEMU 门禁均通过。移除 `hostfwd` 是因为宿主端口 5555 已被其他进程占用，与被测机制无关。关键输出为：

```text
smoke-envelope: PASS
smoke-envelope-pilot: PASS
smoke-envelope-bench: PASS
smoke-envelope-corpus: PASS
ENVELOPE_CORPUS: ... exfil=6/6 net-only=25/25 stage2=56/56
ENVELOPE_CORPUS: benign completed 20/20, unstable=0, fidelity-bad=0
```

这证明仓库已有门禁当前可运行，但不证明门禁没有漏验。下面多项偏差正是“门禁通过，但论文断言未被门禁实际约束”。

## 2. 已对齐部分

以下部分有明确代码和本次动态验证支撑：

1. `kernel/ipc/envelope.c` 确为 670 行，实现了 policy snapshot、共享预算、shadow、过期、撤销、统计和审计主体。
2. Linux ABI 的 `openat`、`socket`、`pipe2`、memfd/eventfd/timerfd/signalfd、`shmat`、SCM_RIGHTS、`pidfd_getfd`、六个文件 I/O 家族及部分 socket/io_uring 路径存在实际 hook。
3. `fork` 共享 envelope，`execve` 保持 attachment；`mksh-flow` 与三段 `pipe-flow` 在本次 QEMU pilot 中通过。
4. `envelope_smoke` 的功能、类型拒绝、R/W 权利拒绝、op/data/time、重开、撤销、SCM、pidfd、shmat 和运行时审计场景本次均通过。
5. canonical 数据头确有 87 个恶意条目（6 exfil、25 net-only、56 stage2）和 20 个 benign 条目；本次 replay 输出 87/87 阻断、20/20 benign 完成。
6. TLA+ 与 Lean 源文件存在；Lean 文件静态可见 13 个 theorem，未出现 `sorry`。

## 3. 阻断级不对齐

### B-01：论文的“双 ABI 统一强制”未实现

- **论文主张**：所有资源获取不论来自 Linux ABI 或 Native ABI，均通过共同 mediator；信封透明适用于任一 ABI（`main.tex:31-35, 93-99, 241-257, 831-834`）。
- **代码证据**：`env_mediate_*` 调用集中在 `kernel/abi/linux/` 和少量由 Linux 路径触达的 core 文件；`kernel/abi/native/` 中没有 envelope mediation 调用。控制面本身也是 Linux ABI syscall 902--906。
- **影响**：Native ABI 进程即使持有 `task->envelope`，其 handle/channel/resource 操作也不会执行论文所述预算和权利检查。“dual-ABI enforcement”与当前实现不符。
- **建议**：要么把论文范围明确收窄为“dual-ABI OS 上的 Linux-ABI envelope”，要么把 mediation 下沉至两套 ABI 必经的对象层并增加 Native ABI 测试矩阵。

### B-02：“每个资源获取/使用路径均被调解”与仓库自己的矩阵冲突

- **论文主张**：mediator 截获 every acquisition/consumption；A1--A10 枚举每条新 authority 路径并映射到实际 hook（`main.tex:97-99, 252-257, 281-306`）。
- **代码证据**：当前生成矩阵为 366 项，其中仅 30 项标记为已调解，另有 **36 项 PLANNED-W2**。其中包括 `socketpair`、file-backed `mmap`、`shmget`、epoll/inotify、POSIX/SysV IPC、mount、ptrace 等（`envelope_coverage.md:5-14`；`gen_envelope_coverage.py:62-83`）。
- **具体逃逸**：`sys_socketpair()` 创建并安装两个 socket fd，全程没有 `env_mediate_acquire`（`kernel/abi/linux/sys_net.c:49-71`）；A4 `mmap` 仍被矩阵标为 PLANNED。
- **影响**：当前实现是“若干选定 choke points 的原型”，不是论文表述的完整 mediator。允许 SOCKET 之外的 policy 仍可通过未调解 `socketpair` 获得 socket authority。
- **建议**：投稿前将 PLANNED 清零或全部 fail-closed；否则把完整性主张、标题/摘要和威胁模型同步收窄为已实现 syscall 集合。

### B-03：两个 A20 Linux ABI authority 获取 syscall 被错误标为 NA

- **论文主张**：366 个登记 syscall 全部针对资源 authority 被显式分类，防止静默缺口（`main.tex:106-108, 311-316, 386-390`）。
- **代码证据**：syscall 900 `a20_channel_pair` 创建两个 channel endpoint fd，901 `a20_registry_client` 返回 registry endpoint fd，但二者没有 envelope acquisition hook（`kernel/abi/linux/sys_a20_bridge.c:20-76`）。生成矩阵却将二者标记为 `NA`（`envelope_coverage.md:365-366`）。
- **影响**：这是当前矩阵已经漏掉的真实 authority-granting 路径，直接反例于“显式分类杜绝静默缺口”。
- **建议**：将 900/901 定义为 ACQUIRE 并 mediation；在修复前至少 PLANNED/fail-closed，不能标为 NA。

### B-04：覆盖 drift gate 不保证显式分类，而且当前生成器已不幂等

- **论文主张**：新增 syscall 若未显式分类，构建门禁必失败（`main.tex:311-316, 386-390`）。
- **默认 NA 问题**：`classify()` 对不在任何手工集合中的 syscall 无条件返回 `NA`（`tools/gen_envelope_coverage.py:97-108`）。新增 syscall 重新生成并提交后即可通过，无需任何显式判断或理由；B-03 正是实际后果。
- **当前生成失败**：`parse_numbers()` 从 `kernel/include/abi/linux/syscall_nr.h` 搜索 `#define SYS_*`（`gen_envelope_coverage.py:27,89-95`），但该文件现在只 include `core/syscall_nr.h`。以项目指定的 `a20os` conda 环境重新生成时，366 行 syscall number 全部变空并大面积重排，与已提交矩阵产生 366 行替换。
- **门禁局限**：Make target 只“重新生成 + git diff”（`Makefile:918-928`），并不验证分类的正确性、hook 存在性或 PLANNED=0。
- **影响**：机械完备性是论文核心贡献之一，但当前既有逻辑漏洞，也在 HEAD 上发生工具漂移。
- **建议**：建立显式 `NA` allowlist（每项带理由），未知项直接报错；解析唯一 syscall 元数据源；把分类与 hook declaration/测试映射交叉验证；CI 强制 PLANNED 或高风险 NA 审批。

### B-05：descriptor transfer 并未实现论文与 Lean 所证明的权利来源约束

- **论文主张**：transfer grant 为 `request ∩ receiver-cap`，不得超出 request/source；Lean 进一步声称执行 delegated right 需要 delegator grant 与 receiver cap 同时成立（`main.tex:336-349, 650-668`）。
- **代码证据**：`env_mediate_acquire_gfd(int gfd)` 没有 request/source-rights 参数。它为 FILE 硬编码 `READ|WRITE|STAT|SEEK`，socket 再加 `CONNECT|ACCEPT`，最终只计算 `want & receiver-cap`（`kernel/ipc/envelope.c:209-247`）。
- **模型偏差**：TLA+ 同样把 `WantTransfer` 固定成全 R/W（`Envelope.tla:42,85-96`）；Lean 的 `xferGrant(req, cap)` 和 delegation theorem 对抽象 `req/delegator-rights` 成立，但 C 路径没有实现这些输入（`BudgetLattice.lean:81-114,175-205`）。
- **影响**：C 实现只能证明“不超过 receiver cap”，不能证明“不超过 sender shadow/request”。论文的 no-fabrication、delegation-chain 和 transfer-clamping 叙述没有代码对应点。
- **建议**：传递源 shadow rights/请求掩码并计算 `source ∩ request ∩ cap`；增加“只读 source 经 SCM/pidfd 传入后 write 必须失败”等反例测试；随后再保留现有 Lean 结论。

### B-06：create-fork-enter-exec 模式允许 inherited fd 绕过 type 与 rights

- **论文主张**：类型、方向和预算政策透明包裹 unmodified binary；每次 authority 使用由 shadow rights 检查（`main.tex:26-35, 320-334, 360-366`）。
- **代码证据**：进入 envelope 前继承的 fd 没有 shadow。`env_mediate_use_dir()` 对此类 fd 明确“stay allowed but budget-accounted”，且不执行 direction 检查（`kernel/ipc/envelope.c:329-354`；头文件也注明 exempt，`kernel/include/ipc/envelope.h:128-140`）。
- **影响**：supervisor 未关闭的 inherited socket 可在“no SOCKET” envelope 内继续使用；继承的可写文件可绕过 class/right acquisition checks。这与论文推荐的 create-fork-enter-exec 部署模式直接相交。
- **建议**：`enter()` 时枚举并导入/裁剪现有 fd，或默认关闭/拒绝 grandfathered authority；若保留兼容模式，必须作为显式 policy flag 与论文限制披露。

### B-07：论文列出的 STAT/SEEK/CONNECT/ACCEPT 权利并未按操作强制

- **论文主张**：per-class ceiling 包含 read、write、stat、seek、connect、accept（`main.tex:80-86, 318-334`）。
- **文件权利**：use mediator 只有 R/W/无方向三态；`lseek` 直接调用 VFS，无 envelope hook（`kernel/abi/linux/sys_fs.c:625-638`），覆盖矩阵也把 `lseek`、`fstatat`、`fstat`、`statx` 标为 NA（`envelope_coverage.md:80,97-98,295`）。STAT/SEEK 只在 acquisition 时被全量要求，不能形成运行时 operation ceiling。
- **socket 权利**：`socket()` 固定请求 CONNECT|ACCEPT|R|W|STAT（`sys_net.c:29-46`），因此 connect-only 或 accept-only cap 连 socket 都无法创建；`bind/connect/listen` 仅调用无方向的 `env_mediate_use(...,0)`，从不检查 CONNECT/ACCEPT（`sys_net.c:74-107`）。accepted fd 又固定请求所有 socket rights（`sys_net.c:125-138`）。
- **影响**：论文展示的细粒度 rights lattice 在 C 中实质上主要只有 FILE/SOCKET 的 R/W use 检查和 acquisition-time all-or-nothing cap。
- **建议**：为每类控制操作传入明确 required-right；新 socket/accept 的 shadow 从请求、listener 与 cap 派生，而非固定全 rights。

### B-08：io_uring 的“FAILCLOSED/统一调解”分类不成立

- **论文主张**：A9 fixed-file fail-closed，A10 execution-point charge；覆盖矩阵把 `io_uring_setup` 和 `io_uring_register` 都计入 FAILCLOSED（`main.tex:380-390`；`envelope_coverage.md:319-321`）。
- **代码证据**：`sys_io_uring_setup()` 在 envelope 下仍创建并返回 ring fd，未走 acquisition mediation（`kernel/abi/linux/sys_io_uring.c:47-75`），因此不能把整个 syscall 计为 fail-closed。`io_uring_register` 只拒绝 `IORING_REGISTER_FILES`；`IORING_REGISTER_EVENTFD` 仍允许注册（`kernel/fs/io_uring.c:431-471`）。完成通知更直接调用 `vfs_write_file()`，没有 envelope use charge/right check（`io_uring.c:413-425`）。
- **影响**：已调解项计数被高估，并存在 eventfd consumption 绕过。
- **建议**：ring fd acquisition 正常分类/调解；eventfd registration 按 authority transfer 处理，通知写入也在 execution point 计费；矩阵按 opcode 而非整个 syscall 粗分类。

### B-09：pilot 摘要结论与测试预期相反，Landlock 基线还存在生命周期错误

- **论文冲突**：摘要称 envelope 同时完成 benign install 并“blocks all four tested attack classes”（`main.tex:36-42`），正文表和代码却明确规定 A3 path escape 在 envelope 下成功（`main.tex:468-481`；`envelope_pilot.c:304-308`）。本次实测也得到 `A3-escape/envelope PASS (rc=0)`，其中 `rc=0` 就是逃逸成功。
- **Landlock 基线风险**：pilot 在 `landlock_restrict_self()` 后立即 `close(rs)`（`envelope_pilot.c:78-102`）。内核把该对象地址写入 `task->landlock_rulesets`（`kernel/ipc/landlock.c:147-166`），但 fd close callback 即使识别到活动 ruleset，仍无条件 `kfree(rs)`（`landlock.c:51-63`）；后续 enforcement 读取该悬空指针（`landlock.c:190-198`）。
- **影响**：摘要的四类阻断是事实错误；20-cell Landlock 比较是在潜在 use-after-free 上运行，当前 PASS 不能建立基线有效性。该实现也是 A20OS 的 Landlock subset，而不是 commodity Linux Landlock，论文应明确。
- **建议**：摘要改为“阻断 3/4，A3 需与 Landlock 组合”；修复 ruleset ownership/refcount 后重跑完整矩阵并保存原始结果。

### B-10：66 个恶意包 exact-execution 的关键结论无法由仓库复现或审核

- **论文主张**：seeded random 100 包、66 个运行、25 个触网、ENV 全部在 socket 阻断、NONE 触达 live C2，且无 benign denial（`main.tex:43-48, 494-520`）。
- **仓库缺失**：没有提交 sample manifest、随机种子/抽样脚本、样本 hash、逐样本退出状态、原始串口日志、mediator counter 汇总、pcap/listener 记录或结果 JSON/CSV；也没有 exact-execution CI/Make gate。仓库内无法从数据重新得到 66/25/live-C2 这些数值。
- **runner 丢证据**：`run_all.sh` 每个样本覆盖 `/tmp/o_none` 与 `/tmp/o_env`，最终只保留最后一个样本；它 grep errno 字符串，但 `ENVWRAP-STATS deny_type=...` 不含这些字符串，因此论文所称的 counter attribution 没有被 runner 输出或汇总（`tools/corpus/gen_exec_corpus.py:114-140`；`user/cmds/core/envwrap.c:80-97`）。
- **版本未固定**：`packages/world/pynode.world` 只列 `python3`、`nodejs`、`npm`，没有锁定论文中的 Node.js 24.18.1 / CPython 3.12.14（`pynode.world:11-20`）。
- **影响**：当前仓库只能支持“存在 exact runner 原型”，不能独立支持论文的 66/25/live-C2 实证结论。
- **建议**：提交不含恶意 payload 的可公开 manifest（ID/hash/生态/entry/结果）、固定 seed 与抽样脚本、锁定镜像 digest/package versions、逐样本结构化输出、counter/pcap 摘要及一键 gate。

## 4. 高优先级不对齐

### H-01：“unmodified install scripts/packages”措辞过强

`gen_exec_corpus.py` 会改写每个路径组件、排除 `package_info*`、跳过大于 8 MiB 的文件（`lines 20-41`）。npm lifecycle 命令被从 `package.json` 提取后逐条交给 `sh -s`，不是由 npm 执行，因此缺少 npm lifecycle 环境、依赖与 shell 语义；PyPI 固定走 `python3 setup.py install`（`lines 92-107`）。更准确的表述应是“在经过 FAT32 staging 转换的 package tree 上执行原始 script body”，而不是 unmodified package execution。

### H-02：canonical replay 的 PASS 条件不要求 ENV 阻断

论文称 NONE 87/87 成功、ENV 87/87 阻断、三次稳定（`main.tex:541-551`）。当前本次运行确实输出 87/87，但 harness 的失败条件只检查 NONE fidelity、稳定性和 benign completion；ENV block rate 仅打印，不参与 `failures`（`user/cmds/core/envelope_corpus.c:300-365`）。因此未来即使 ENV 0/87 阻断，也可能显示 `ENVELOPE_CORPUS: PASS`。应把论文断言写进 gate。

### H-03：性能表既不可复现，read 项也没有测量“每次读取 64B”

- `bench_read()` 对同一 64-byte 文件连续调用 20,000 次 `read(fd, ..., 64)`，没有 rewind；只有第一次传输 64B，余下 19,999 次为 EOF（`envelope_bench.c:72-82,177-186`）。名称 `read-64B` 因而不准确。
- mediator 在实际 I/O 前按请求长度预扣，所以 EOF 仍扣 64 bytes（`kernel/abi/linux/sys_fs.c:286-300`；`envelope.c:356-371`）。该项主要测量“受调解的 EOF syscall”，不是 64B 数据传输。
- on-phase 的 `socketpair` 在进入 envelope 后创建，却因 B-02 未调解而成为 untracked fd；注释称其为 grandfathered 不准确（`envelope_bench.c:145-151`）。
- 论文只报告单个 loop，无重复次数、误差、置信区间或 raw log。相同 gate 本次复跑与论文数值差异显著：

| 操作 | 论文 | 本次复跑 |
|---|---:|---:|
| open+close | +4.1% | +2.6% |
| read-64B | +29.2% | +7.0% |
| write-64B | +17.7% | +9.2% |
| lseek control | +1.1% | -3.4% |
| sendto+drain | +9.1% | +4.7% |

应修复 read workload、加入 warm-up/多次独立重复/统计区间，并保存机器可读原始结果；论文表需由固定 artifact 生成。

### H-04：论文声称的 G2 `wget-blocked` 测试不存在

论文 `main.tex:567-570` 和 `docs/research/verification/STATUS.md:56-63` 都声称有真实 `wget` cell，但 `envelope_pilot.c` 只实现 G1 `mksh-flow` 和 G4 `pipe-flow`；`envelope_smoke.c`、pilot、bench、corpus 中均无 wget 调用。本次 pilot 日志同样只有这两个额外 real-binary cell。应补测试或删除 G2 段落/STATUS 声明。

### H-05：data budget 是“请求字节预扣”，不是“实际传输累计”

论文前部定义为 cumulative bytes transferred（`main.tex:85-86`），而代码在 read/write/send/recv 执行前按 `count/len` 扣减，短读、EOF 甚至后续 I/O 失败都不会退款（`envelope.c:363-370`；`sys_fs.c:286-317`；`sys_net.c:195-235`）。论文实现段虽提到 conservative pre-charging，但全文指标名称和语义应统一为“attempted/requested bytes charged”，或实现 post-I/O reconcile。

### H-06：SCM 传递后的对象类型可能退化成 FILE

SCM delivery 会分配新的 global fd，原创建点的 kind 记录仍在 sender gfd。`env_kind_of()` 只能额外识别 socket，其他未登记的新 gfd 一律回退 FILE（`kernel/ipc/envelope.c:405-437`）。因此 PIPE、EVENT_QUEUE、TIMER、channel 等经 SCM 传入后可能按 FILE cap 安装，违反 type-aware transfer 叙述。应让 fd duplication 携带 object kind，而不是按新 fd 猜测。

### H-07：envelope registry 没有释放 owner 初始引用

`env_create()` 将 refcount 设为 1 并写入最多 1024 项的 registry（`envelope.c:470-516`）；`enter()` 再加 task ref，task exit 只释放 task ref（`lines 519-538, 460-466`）。仓库没有 destroy/close API 释放 registry 的初始引用，因而 `env_destroy()` 正常情况下不可达，连续包装 1024 次后会 `-EBUSY`。这与 CI/corpus runner 的长期 deployability 不符。应增加 owner close/destroy 生命周期，并测试 slot reuse。

### H-08：KILL_ON_EXPIRE 最多只杀 32 个 attachment

`env_kill_tasks()` 使用固定 `pids[32]` 且扫描条件为 `n < 32`（`envelope.c:106-124`）。`kill_sent` 又是一次性 latch，超过 32 个的 attachment 不会在后续补杀（`lines 140-143, 558-560`）。论文/头文件所述“所有 attached task”不成立。应分批遍历或持有安全 task references 后完整发送。

### H-09：形式化的 cross-envelope budget 结论不是 C 实现性质

Lean 定义 `scmRecvCharge(s,r)=min(s,r)` 并证明不超过双方（`BudgetLattice.lean:116-146`）；C receive path 固定只从 receiver 扣 1 op，没有 sender-side charge/input（`envelope.c:214-247`）。论文却称“no sender coupling”仍“by construction”满足 `charge <= min(sender,receiver)`（`main.tex:662-668`）：若抽象 sender charge 为 0 而 C 扣 1，该不等式显然不成立。应将 theorem 标为未来规范，或实现 sender budget coupling；不能声称当前 C 满足该双边 bound。

## 5. 中优先级漂移与不一致

| ID | 不对齐 | 证据与影响 |
|---|---|---|
| M-01 | syscall 数字在论文内部和代码之间漂移 | 论文同时使用 Linux ABI 361、365、366。当前 `syscall_table.def` 是 366。Native 表实际有 142 个 `A20_NATIVE_SYSCALL`，头文件常量是 135，论文写 136（`main.tex:94-96,246-249,312,387`；`kernel/include/abi/native/syscall_nr.h:182`）。应从表自动生成数字。 |
| M-02 | coverage 统计过期 | 论文写 20 mediated / 46 PLANNED / 299 NA，并在 limitations 仍写 46、把 sendto/recvfrom 列为 planned（`main.tex:311-316,817-821`）；当前矩阵为 30 / 36 / 300，sendto/recvfrom 已标 USE。 |
| M-03 | control-plane syscall 范围写错 | `main.tex:360-362` 称 902--905 包含 audit；实际 stats 是 905，audit 是 906（`sys_a20_bridge.c:80-130`）。 |
| M-04 | TLA+/Lean 行数和配置描述过期 | 论文写 TLA+ 199 行、Lean 200 行；当前分别 214、205。`Envelope.cfg` 实际列 6 个 invariants（含 DenyAfterExpiry）和 4 个 PROPERTY（另含 ExpiryLiveness/KillLiveness），而论文只描述 5+2。 |
| M-05 | formal 结果缺少可审计输出 | 仓库没有 `tla2tools.jar`、TLC raw log、Lean build log/lockfile；当前 PATH 也没有 `lean`。本次只能静态确认模型/13 theorems 存在，不能从仓库复跑论文的 27,829/4,620 states 或 Lean v4.33.1 结果。 |
| M-06 | runtime audit 不是“全部 model-checked invariants” | `env_audit()` 只查 TypeAllowed、RightsSubCap、预算上界与 task pointer consistency（`envelope.c:607-668`），不重查 NoResurrect、KillOnce、ClockBounded、DenyAfterExpiry 或 liveness。`main.tex:675-680,849-850` 应保持窄表述。 |
| M-07 | zero-budget API 文档与实现相反 | 头文件称 zero budget 表示 unlimited（`kernel/include/ipc/envelope.h:48-58`）；实现中 `remaining_ops==0` 会立即拒绝，`remaining_data < bytes` 也会拒绝（`envelope.c:149-169,356-370`）。虽非论文主结论，但会影响外部 policy 解释与复现实验。 |
| M-08 | A1 “exfil succeeded”指标只要求 socket 创建 | pilot 会调用 `sendto`，但忽略其返回值，只要 `socket()` 成功就返回攻击成功（`envelope_pilot.c:157-184`）。应称“network channel acquisition succeeded”，或实际验证数据被 listener 接收。 |

## 6. 门禁覆盖不到的主张

| 论文主张 | 当前最近的门禁 | 门禁实际未验证内容 |
|---|---|---|
| 双 ABI 统一 mediation | `smoke-envelope`（Linux ABI image） | 没有 Native ABI envelope 场景。 |
| 366 syscall 无静默缺口 | `check-envelope-coverage` | 默认 NA；不验证 hook；HEAD 上生成器不幂等；允许 PLANNED=36。 |
| pilot 阻断四类攻击 | `smoke-envelope-pilot` | A3 的 PASS 条件恰是逃逸成功；Landlock ruleset 生命周期有 UAF 风险。 |
| exact 66 / net 25 / live C2 | 无 | 没有结构化结果、manifest、版本锁定、网络证据或 CI target。 |
| canonical ENV 87/87 | `smoke-envelope-corpus` | 当前输出为 87/87，但 PASS 条件不要求 ENV block。 |
| G2 wget real binary | `smoke-envelope-pilot` | harness 无 wget cell。 |
| 论文性能数字 | `smoke-envelope-bench` | workload 的 read 标注错误；只有单次 loop；本次数字与论文显著不同。 |
| transfer/delegation theorem 对应 C | smoke + Lean 源文件 | 没有 source/request rights 参数，也没有 sender budget coupling/refinement test。 |

## 7. 建议修复顺序

1. **先修论文真实性**：立即改摘要的“all four”、删除/补齐 wget、统一 syscall/coverage/形式化行数，并把范围改为 Linux ABI + 当前 hook 集合。
2. **修安全语义**：堵住 `socketpair`、900/901、io_uring eventfd 等 acquisition/use 漏洞；处理 inherited fd；补齐 STAT/SEEK/CONNECT/ACCEPT。
3. **修 transfer/refinement**：把 source/request rights 与 sender budget 真正接入 C，再用反例测试连接 Lean/TLA+ 与实现。
4. **修基线和生命周期**：Landlock ruleset refcount、envelope registry owner ref、超过 32 tasks 的 revoke/expiry。
5. **修机械门禁**：未知 syscall 默认失败、显式 NA 清单、解析 core syscall number source、PLANNED policy、hook/test 交叉检查。
6. **重做可复现实验包**：exact execution 生成结构化结果和公开 manifest；benchmark 修正 workload 并做多次统计；将论文表格由 artifact 自动生成。
7. **最后重跑并固化证据**：四个 QEMU gates、exact corpus、TLC、Lean；提交版本、命令、raw/summary logs 与环境 digest。

## 8. 最终判断

当前仓库足以支持以下较窄结论：

> A20OS 实现了一个面向 Linux ABI 的 capability-envelope 原型；它在若干已接入的文件、socket、IPC 与 io_uring 操作上执行 type/RW/op/data/time 检查，并在仓库自带的 smoke、pilot 和 canonical replay 场景中展示了预期行为。

当前仓库**不足以支持**以下论文现有强结论：

- 两套 ABI 的所有资源路径均由同一 envelope mediator 完整覆盖；
- 366 syscall 的机械 drift gate 已杜绝静默 authority 缺口；
- descriptor transfer 保证不超过 source/request，且满足 sender/receiver 双边预算 bound；
- pilot 中 envelope 单独阻断四类攻击；
- 66 个真实包、25 个触网样本和 live C2 结果可由仓库独立复现；
- 论文中的性能数值是由当前 benchmark 稳定、正确地测得；
- 现有形式化结论已经与 C 实现建立足够的对应关系。

因此整体对齐程度应定性为：**核心原型对齐，核心论文论证链未对齐。**
