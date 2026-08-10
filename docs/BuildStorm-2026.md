# A20OS 2026 BuildStorm 设计与优化说明

## 1. 评测目标与提交契约

A20OS 同时支持 RISC-V64 与 LoongArch64。决赛提交在项目根目录执行 `make all`，
生成 ELF 格式的 `kernel-rv`、`kernel-la`，以及分别供两种架构启动使用的
`disk.img`、`disk-la.img`。两个内核均按 8 vCPU 配置编译。当前决赛根文件系统
设计将评测机提供的官方 ext4 盘直接挂载到 `/`，将只含 A20OS bootstrap 用户态、
入口标记和决赛 runner 的 FAT 盘挂载到 `/a20`。
决赛内核固定使用 `DRIVER_DEPLOYMENT=embedded`，确保评测命令默认提供的 legacy
VirtIO-MMIO 块设备能在 DriverStore 挂载前完成探测；普通开发构建仍保留 generic
动态驱动部署方式。

`/a20/final_contest.sh` 支持 `all`、`auto`、`cagent`、`buildstorm` 和定点 probe
分组。`all`/`auto` 先通过稳定路径直接运行已知的 CAgent、BuildStorm，再扫描并
串行运行其余 `/glibc/*_testcode.sh`，已知分组不会重复执行。测试脚本看到的 `/`
就是官方 ext4 根；runner 只使用 `/a20/mksh` 作为解释器。全部测试结束后执行
`sync` 并主动关机。初赛入口没有删除，仍可通过 `make preliminary-all`（或单独的
`make contest-rv`、`make contest-la`）构建；初赛盘与决赛盘使用不同入口标记和
脚本。

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
页表和等待队列的组合放大。截至最新完整证据提交 `f9732348`，优化集中在以下可由
计数器和短 probe 证明的热区；当前源码保留了这些实现：

1. page cache、VFS 和 ext4 热路径从固定容量全表扫描改为索引查找，减少小文件
   close/writeback、truncate、目录查找和块分配的重复工作；
2. 空闲 vCPU 从调度器忙轮询改为安全等待，降低 TCG 多线程下无用的宿主 CPU
   争用；
3. mm 维护 active CPU 集合，只向真正运行过该地址空间的 CPU 发起 TLB
   shootdown，同时保留释放旧 frame 前的同步正确性；
4. 缩减进程表和 fdtable 的固定范围扫描，避免 fork/exec/wait 与大量短生命周期
   子进程按最大容量付费；
5. 去除 VirtIO poll-only 等待中的重复扫描和重复 progress callback，以指数退避
   降低空轮询；block page-cache fill 使用 hash shard，并跳过 ext4 bitmap 中连续
   的已占用字节；
6. ext4 vnode cache 改为 bucket 索引和 free list，扩大 block page-cache 与 VFS
   dcache 的元数据工作集容量，并保留既有写回、dirty generation 和淘汰边界。

所有性能改动都保留真实 `/proc/uptime` 计时，不跳过编译、不伪造核心数、产物或
输出。正式计时只覆盖官方的
`cargo xtask arceos build -p arceos-helloworld --arch <arch>`。

## 4. 实验结果

### 4.1 固定快照与证据边界

性能数字按提交固定，不能把旧提交的运行结果称为审计基线 `e33c3219` 的结果：

| 快照 | 证据性质 | RISC-V64 elapsed_s | LoongArch64 elapsed_s |
|---|---|---:|---:|
| `d5ae5a16` | 历史优化前正式样本 | 2551.12 s | 2315.48 s |
| `f3e3ce48` | 第一批优化与正确性修复后的历史样本 | 1965.67 s | 1883.74 s |
| `f9732348` | 最新完整、干净、双架构平台运行 | 1483.00 s | 1399.13 s |
| `e33c3219` | 本文审计源码基线 | 无匹配完整运行 | 无匹配完整运行 |

历史 Linux 基线为 RISC-V64 `638.37 s`、LoongArch64 `565.21 s`。`d5ae5a16`
与 `f9732348` 使用相同 workload 和名义 guest 配置，但 QEMU 分别为 10.2.2 和
10.0.2；由两者计算的 1068.12/916.35 秒差值及 1.72x/1.65x 仅是跨版本演进指标，
不是可直接比较的 A20OS 内部 speedup。`d5ae5a16` 与 Linux 使用相同 QEMU、配置和
workload，但 runner、启动镜像和控制镜像不同；`f9732348` 相对 Linux 的
2.32x/2.48x 还跨 QEMU 版本，两组都必须按各自边界解释。

### 4.2 最新完整平台证据

最新完整证据来自干净提交
`f973234811c815a4d6202ae694653f11638fe978`：

```text
.eval-state/2026/platform-final-20260810T001000Z-f9732348/REPORT.md
```

环境为官方 `zhouzhouyi/os-contest:20260510` 容器与 2026 发布镜像，容器内 QEMU
为 10.0.2，8 GiB、8 vCPU、`tcg,thread=multi`，每个架构使用独立 qcow2 overlay
和 3000 秒外层 watchdog。结果如下：

| 架构 | BuildStorm elapsed_s | 编译产物 | BuildStorm | CAgent | QEMU |
|---|---:|---:|---:|---:|---|
| RISC-V64 | 1483.00 s | 1,681,000 bytes | 180/180 | 10/10、199.10/200 | exit 0，主动关机 |
| LoongArch64 | 1399.13 s | 1,714,568 bytes | 180/180 | 10/10、199.10/200 | exit 0，主动关机 |

该次冷构建的提交产物哈希为：

| 产物 | SHA-256 |
|---|---|
| `kernel-rv` | `982606d93c79f834c0ccf14b1df278f4cda452076b6f03d62b34257272f3adbb` |
| `kernel-la` | `0fc2e0a38801a63c72352d63da81a76e7d2a790bcdd104577a566f11779ad2a7` |
| `disk.img` | `bef80d3f233a0ec8bc98dd61a03a717074a2dc827a313ae0d97a44987027eb63` |
| `disk-la.img` | `bd651ea6b3f2e9b8a1d033cc678ec82aeae48de0d2a80a869d7662eefc71f5d7` |

BuildStorm 的设计文档 20 分是人工项，不包含在 180 分 judge JSON 中。该平台
运行对每个架构只提供一次同代码完整样本，因此仍不满足仓库规定的阶段 8
“同一代码连续成功 2 次”规则，不能据此宣布阶段 8 或 contest 完成。

### 4.3 审计源码状态

本文审计基线是 `e33c3219dcf5e7f9d1476eeedda99bfb0c619eb1`。在 `f9732348`
完整运行之后又有 6 个提交，依次修改 rootfs overlay 生成、RISC-V 内存映射、
决赛测试发现、官方 ext4 根挂载、final-root mount 隔离和 RISC-V 辅助盘暴露。
这些修改形成了第 1 节所述审计设计，但没有与 `e33c3219` 匹配的完整干净双架构
平台运行，正式状态为未验证。`f9732348` 的分数、耗时和产物哈希不得外推到
`e33c3219`。

## 5. 复现方法

```bash
# 在官方构建环境的干净仓库根目录
make all

file kernel-rv kernel-la
mdir -i disk.img ::
mdir -i disk-la.img ::
```

本地按官方评测盘复现单个架构时，可使用仓库已有正式入口；它会记录所用压缩
镜像和恢复后 base 的 SHA-256，以实际 archive hash 隔离 base cache，检查缓存
base 与其伴随 hash 一致并保持 `0444`，再创建独立 qcow2 overlay、保存日志和
metadata：

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

当前 `tools/run_final_eval.sh` 记录实际 archive hash，但没有把它与上述发布值做
硬编码比较。正式复现时必须独立比较 metadata 中的
`official_image_archive_sha256` 与上述期望值；不能仅凭 runner 成功声称发布
SHA-256 已验证。

关键实现与验证提交包括：阶段 7 并行编译修复 `92ae635c`，性能批次
`1d41d449`，持久化正确性修复 `86d0817f`，以及合入上游后的验证基线
`f3e3ce48`；后续平台性能收口的最新完整验证提交为 `f9732348`。完整原始日志、
metadata 和 judge JSON 保存在开发工作区的 `.eval-state/2026/`；该隐藏目录不
属于提交运行依赖，本文件保留评测所需的公开设计与复现摘要。审计基线 `e33c3219` 的正式
复验仍待执行。

## 6. AI 使用说明

开发过程中使用 OpenAI Codex 辅助代码审查、故障假设整理、测试编排和证据文档
起草。内核改动、根因结论和性能数字均以干净提交上的真实编译、QEMU 日志、
产物检查及 judge 结果为准；没有修改官方测试脚本或数据来制造通过结果，也没有
伪造计时、系统时钟、核心数或评测输出。
