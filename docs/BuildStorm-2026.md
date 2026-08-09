# A20OS 2026 BuildStorm 设计与优化说明

## 1. 评测目标与提交契约

A20OS 同时支持 RISC-V64 与 LoongArch64。决赛提交在项目根目录执行 `make all`，
生成 ELF 格式的 `kernel-rv`、`kernel-la`，以及分别供两种架构启动使用的
`disk.img`、`disk-la.img`。两个内核均按 8 vCPU 配置编译；辅助盘只包含 A20OS
用户态和决赛串行 runner，官方 ext4 测试盘仍由评测机单独挂载。
决赛内核固定使用 `DRIVER_DEPLOYMENT=embedded`，确保评测命令默认提供的 legacy
VirtIO-MMIO 块设备能在 DriverStore 挂载前完成探测；普通开发构建仍保留 generic
动态驱动部署方式。

runner 启动后扫描官方测试盘 `/glibc/*_testcode.sh`，逐个进入官方根文件系统
运行，不并行执行测试组。全部测试结束后执行 `sync` 并主动关机。初赛入口没有
删除，仍可通过 `make preliminary-all`（或单独的 `make contest-rv`、
`make contest-la`）构建；初赛盘与决赛盘使用不同入口标记和脚本。

## 2. 功能正确性问题与修复

### 2.1 进程、VFS、ELF 与架构上下文

早期完整 Cargo/rustc 负载暴露了普通小型测试不容易覆盖的组合问题，包括 cwd
与 chroot 路径语义、目录项创建后的立即可见性、动态 ELF 文件页映射、TLB 与
`mremap` 一致性，以及 RISC-V64/LoongArch64 的 TLS、信号和向量上下文保存。
这些问题均按 Linux ABI 语义修复，并使用独立的定点 probe、并发 rustc 和官方
测试组交叉验证。

### 2.2 并行编译中的旧 TLB frame 复用

8 核/8 job 全量编译曾出现 RISC-V64 rustc ICE，以及 LoongArch64
SIGILL、SIGSEGV 和地址异常。根因是清除或替换 PTE 后，旧 frame/page-cache
引用可能在远端 TLB shootdown 完成前被释放并复用。修复引入 per-mm TLB
invalidation transaction，延迟释放旧 backing；LoongArch64 同步等待远端
shootdown，并统一处理 VMA deferred flush、slab、block-cache 与 buddy 的相关
并发边界。

### 2.3 ext4/JBD2 与持久化顺序

新版官方镜像带有需要恢复的 JBD2 journal。A20OS 对已支持的
`64bit + checksum v3 + revoke` 组合进行校验后 replay，对未知特性或损坏日志
fail closed。ext4 目录项拆分同时补齐了最小头部、对齐和块边界校验，避免目录块
尾只剩 4 字节时越界写入。

正式 BuildStorm 后续还暴露了两处高并发持久化损坏：VirtIO packed used ring
的 16 位 `idx` 可能被撕裂读取；block-cache 并发写回可能让旧 ext4 bitmap
晚于新快照落盘。修复固定共享结构布局与读取 barrier，并用 writeback/fill 锁和
dirty generation 保证快照发布、清脏和回写次序。

## 3. 性能设计

BuildStorm 的主要成本不是单一系统调用，而是 Cargo/rustc 对大量小文件、进程、
页表和等待队列的组合放大。当前优化集中在以下可由计数器和短 probe 证明的热区：

1. page cache、VFS 和 ext4 热路径从固定容量全表扫描改为索引查找，减少小文件
   close/writeback、truncate、目录查找和块分配的重复工作；
2. 空闲 vCPU 从调度器忙轮询改为安全等待，降低 TCG 多线程下无用的宿主 CPU
   争用；
3. mm 维护 active CPU 集合，只向真正运行过该地址空间的 CPU 发起 TLB
   shootdown，同时保留释放旧 frame 前的同步正确性；
4. 缩减进程表和 fdtable 的固定范围扫描，避免 fork/exec/wait 与大量短生命周期
   子进程按最大容量付费。

所有性能改动都保留真实 `/proc/uptime` 计时，不跳过编译、不伪造核心数、产物或
输出。正式计时只覆盖官方的
`cargo xtask arceos build -p arceos-helloworld --arch <arch>`。

## 4. 实验结果

环境为官方 `zhouzhouyi/os-contest:20260510` 工具链与 2026 发布镜像，QEMU
9.2.1，8 GiB、8 vCPU、`tcg,thread=multi`。每个正式样本均从只读基础镜像新建
独立 overlay，并使用 3000 秒外层 watchdog。

| 架构 | 优化前 A20OS | 当前 A20OS | 节省 | A20OS 加速比 | Linux 基线 | 当前/Linux |
|---|---:|---:|---:|---:|---:|---:|
| RISC-V64 | 2551.12 s | 1965.67 s | 585.45 s | 1.30x | 638.37 s | 3.08x |
| LoongArch64 | 2315.48 s | 1883.74 s | 431.74 s | 1.23x | 565.21 s | 3.33x |

当前两次正式样本均获得 toolchain 8/8、minibuild 12/12 和全量编译 40/40；
RISC-V64 时间分 94/120，技术项合计 154/180，LoongArch64 时间分 120/120，
技术项合计 180/180。产物分别为 1,681,000 bytes 和 1,714,568 bytes。

功能侧还完成了双架构 CAgent 10/10、阶段 5 工具链矩阵、并行 rustc、块 I/O
压力、VFS/MM/调度/futex/proc 压力和资源生命周期检查。上述正式性能数字各取
一个有效冷启动样本，没有通过重复运行挑选较好成绩。

## 5. 复现方法

```bash
# 在官方构建环境的干净仓库根目录
make all

file kernel-rv kernel-la
mdir -i disk.img ::
mdir -i disk-la.img ::
```

本地按官方评测盘复现单个架构时，可使用仓库已有正式入口；它会校验官方压缩
镜像 SHA-256、创建只读 base 与独立 qcow2 overlay、保存日志和 metadata：

```bash
FINAL_EVAL_TIMEOUT=3000 make final-eval-rv-buildstorm
FINAL_EVAL_TIMEOUT=3000 make final-eval-la-buildstorm
```

judge 必须在项目规定的 conda 环境中运行：

```bash
conda run -n a20os python \
  contest/testsuits-for-oskernel/judge/judge_buildstorm-glibc.py serial.log
```

官方发布镜像 SHA-256：

```text
riscv64     cba87f43ae569bcf2b8e4614f75cec1bf51bedb2804626fe466fcce3861df6f1
loongarch64 2c411447274fbd83505d2fac505a5d9e8ed8ff3bdfc3d2d6cbdb8f61ff7d90d2
```

关键实现与验证提交包括：阶段 7 并行编译修复 `92ae635c`，性能批次
`1d41d449`，持久化正确性修复 `86d0817f`，以及合入上游后的验证基线
`f3e3ce48`。完整原始日志、metadata 和 judge JSON 保存在开发工作区的
`.eval-state/2026/`；该隐藏目录不属于提交运行依赖，本文件保留评测所需的公开
设计与复现摘要。

## 6. AI 使用说明

开发过程中使用 OpenAI Codex 辅助代码审查、故障假设整理、测试编排和证据文档
起草。内核改动、根因结论和性能数字均以干净提交上的真实编译、QEMU 日志、
产物检查及 judge 结果为准；没有修改官方测试脚本或数据来制造通过结果，也没有
伪造计时、系统时钟、核心数或评测输出。
