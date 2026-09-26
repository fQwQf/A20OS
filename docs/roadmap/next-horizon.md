# 下一阶段生态纵深评估（2026-09 记录）

本文记录四个大型方向的现状、成本评估与建议切入点。均为多日级工程，
本次改进周期未实现，此处给出事实基础与路径，供后续迭代使用。

## 1. netfilter / 防火墙

- 现状：内核网络栈为 lwIP（kernel/net/lwip_stack.c 全局锁串行化），
  无 netfilter/conntrack 任何痕迹（kernel/net 零匹配）。
- 成本评估：框架级特性。最小可用切片是在 `a20_lwip_poll` 数据面的
  ip4_input/ip4_output 进出点挂 hook 链（参考 `socket_alg.c` 的
  AF_ALG 注册模式），先交付"规则表 + 丢弃/放行 + /proc/a20/netfilter
  计数器"的可观察骨架，再谈 NAT/conntrack。估计 3-5 天。
- 前置：无硬阻塞；hook 点须遵守 docs/net/network-lock-contract.md 的
  deferred bottom-half 规则。

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
- ACL 依赖 xattr 先行。

## 4. 电源管理

- 现状：无 cpufreq/suspend/resume/CPU 热插拔/ACPI；QEMU 为主要
  运行环境，电源管理收益主要在物理板（VisionFive2、LS2K1000）。
- 成本评估：与板级支持深度绑定。最小切片：WFI idle 已有
  （arch_cpu_relax/wfi 存在），可补 cpuidle 统计与 SBI suspend
  （riscv64）。真机验证是瓶颈——没有 CI 真机门禁前不建议投入。

## 排除项

- exFAT、btrfs 类 CoW 文件系统：无需求驱动，不立项。
- KASLR：评估结论见 docs/security/hardening.md（需 PAGE_OFFSET
  运行时化前置重构，暂缓）。
