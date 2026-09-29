# 下一阶段生态纵深评估（2026-09 记录）

多数为多日级工程。标注「已落地」的部分给出实现事实与验证入口，其余给出成本与路径。

## 1. netfilter / 防火墙（第一切片已落地）

- 现状：已实现可用的 IPv4 包过滤（`kernel/net/netfilter.c`）。hook 挂在
  `a20_lwip_process_netif_rx_tx_locked`（进）与 `a20_lwip_linkoutput`（出），
  即 `next-horizon.md` 当初评估的两个数据面点位。规则表 32 条，按方向、
  协议、源/目的地址、源/目的端口匹配，每条独立可通配；accept/drop 判决；
  每规则与全局计数器。
- 控制面：`/proc/a20/netfilter`。读为策略+计数器+规则表，写接受
  `add <rule>` / `del <n>` / `reset`。格式非法的规则被拒绝而非静默接受，
  避免一次拼写错误悄悄放宽或收紧策略。
- 启动时无规则、默认策略 accept，因此**未配置的机器行为与引入前完全一致**，
  不会让既有部署回归。
- 验证：`make smoke-netfilter`（`user/cmds/net/netfilter_test.c`）。除控制面
  外，断言匹配 drop 规则的 UDP 发送确实让 `out_dropped` 增长。实测
  `dropped=3 out_packets=3`，证明 hook 在真实数据面执行而非空过。
- 仍缺：conntrack 与 NAT。二者需要跨包状态，是独立的一层，不在第一切片内。
  在此之前，本过滤器只能做无状态丢弃，**不能**做端口转发或地址转换。

## 2. 树内动态链接 libc

- 现状：内核 ELF loader 支持 PT_INTERP（kernel/mm/elf.c
  resolve_interp），能跑 Alpine 预编译动态 musl 程序；但树内
  mlibc 只产 libc.a（tools/targets-mlibc.mk），全架构
  `-static -no-pie` 写死。
- 成本评估：工具链工程而非内核工程。最小切片：mlibc 增加
  libc.so/ld.so 构建产物 + 一个动态链接的 hello 冒烟
  （内核 PT_INTERP 路径已被 Alpine 包验证，风险低）。估计 1-2 天。

## 3. ext4 可写 journal / xattr / ACL

- 现状：`kernel/fs/diskfs/ext4_journal.c` 明确 fail-closed 只读回放
  （"does not implement a writable journal"）；VFS 有 xattr 框架
  （kernel/fs/xattr.c）但 diskfs 下零实现；无 ACL。
- 成本评估：journal 写入是正确性敏感工程（崩溃一致性），建议先做
  xattr 落盘（ext4 的 EA 块/inode 内联，1-2 天，可测试性好），
  journal 写入单独立项（一周级，需要断电/崩溃注入测试基础设施）。
- 前置已就位：块层现在有真实的设备 flush（见
  [../security/hardening.md](../security/hardening.md) 的「存储持久性」），
  因此崩溃注入测试有了一个可断言的落点；此前 `fsync()` 连设备都到不了，
  断电测试无从判断成败。
- ACL 依赖 xattr 先行。

## 4. 电源管理

- 现状：无 cpufreq/suspend/resume/CPU 热插拔/ACPI；QEMU 为主要
  运行环境，电源管理收益主要在物理板（VisionFive2、LS2K1000）。
- 成本评估：与板级支持深度绑定。最小切片：WFI idle 已有
  （arch_cpu_relax/wfi 存在），可补 cpuidle 统计与 SBI suspend
  （riscv64）。真机验证是瓶颈；没有 CI 真机门禁前不建议投入。

## 排除项

- exFAT、btrfs 类 CoW 文件系统：无需求驱动，不立项。
- KASLR：评估结论见 docs/security/hardening.md（需 PAGE_OFFSET
  运行时化前置重构，暂缓）。
