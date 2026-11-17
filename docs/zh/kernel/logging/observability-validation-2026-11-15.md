# 可观测性验证（2026-11-15 至 16）

本轮目标是支持设备移植、功能测试、瓶颈分析与优化比较，接口和操作说明见 [设备测试与性能分析](observability.md)。下列软件验收已完成，实板不在本次验证范围内。

全部原始产物在仓库的 `.kernel-build/smoke/observability/`，不作为源码提交。`validation.json` 汇总结果、日志哈希和最终镜像 SHA256；各采集目录有独立 `manifest.json` 和 `serial.log`。用于最终证据的镜像固定归档在 `verified-kernels/`，已逐一核对镜像及全部采集文件的哈希。最终还专项验证了同时检查实际编译能力与请求的采集策略，避免忽略日志级别、控制台或报告策略变更。

| 项目 | 结果 | 证据位置（相对此目录） |
| --- | --- | --- |
| 完整宿主回归 | host-tests 通过；Python 1574 项，2 项跳过 | `final-regression.log` |
| 诊断整体验收目标 | 启用/裁剪构建、ELF、两架构来宾、关闭后 shell 均通过 | `final-integration.log` |
| procfs 特性组合 | Rust 宿主测试、C/Rust 200000 步差分、RISC-V 目标对象通过 | `procfs/` |
| RISC-V64 实际来宾 | 显式窗口、冻结读取、启动/驱动报告、trace 过滤和 marker、32 子进程、文件、关机通过 | `riscv64-guest/` |
| x86_64 实际来宾 | 同上，含 BusyBox shell 重启通过 | `x86_64-guest/` |
| 全部模块关闭 | 编译能力全为 0、报告/trace 节点不存在、控制不支持、shell/关机正常 | `off-guest/` |
| 两架构证据包 | 正常负载完成，IRQ/I/O 分布与锁增量可解析 | `verified-rv-a/`、`verified-x86/` |
| 跨启动对比 | 相同 RISC-V 负载两次独立启动，重新核验冻结快照和文件哈希后比较通过 | `verified-rv-b/`、`verified-comparison.json` |
| 独立模块组合 | 日志/Boot Report/metrics 关闭，移植台账和启动 trace 可用，启动 ID 一致且非空 | `verified-independent/` |
| 失败负载 | exit 7 被保留，采集状态为 failed，之后的 perf/trace 仍保存 | `verified-failed/` |
| 默认启动/屏幕诊断 | off、manual、always、error、配置紧凑模式、logging-disabled、x86 screen 共 7 种通过 | `boot-modes/result.json` |
| 定时器专项 | RISC-V64 与 x86_64 均通过 CPU 抢占、200 ms 空闲超时、计数器窗口、shell 和关机 | `timers-rv-ordered-guest.log`、`timers-x86-ordered-guest.log` |

复现核心验收：

```sh
make -j4 host-tests check-a20-tests
make -j4 check-observability
python tools/qemu/check_boot_report.py --x86
```

本地这次对第二条增加了 `OBSERVABILITY_GATE_DIR=.kernel-build/smoke/observability`。普通 gate 默认放在独立的 `observability-gate` 目录。

专项宿主测试直接编译生产收集器，使用 ASan/UBSan 验证严格控制命令、tick 0、旧窗口拒绝、四个并发生产者与 50 轮停止/重启、时间戳读取期间的停写/重启故障注入、过滤、覆盖、全运行槽位、时钟倒退、JSON 转义、截断、分配失败和冻结后不再变化。锁报告还验证 uint64 最大值、符号解析/格式化不持注册表锁，以及注册覆盖信息。

串口流程通过真实 `SerialTransport` 与宿主 PTY 板卡模拟器运行；断连、超时、原始日志上限和缺失模块通过故障适配器验证。QEMU 的最初采集暴露了 BusyBox 光标响应与下一条输入相互干扰的问题，修正为结束帧、新提示符和终端安静期后才发送下一条；失败现场保留在早期 `diagnose-rv*` 目录。另一个构建组合发现日志关闭时独立事件宏的未使用值告警，已修复并加入参数不求值测试。

最终并发审查将窗口的写入入口、停止标志和排空检查改为顺序一致握手，并在时间戳前后核验代次，以覆盖弱内存序平台上的在途写入边界；关闭状态仍快速返回。

检查实际快照还发现 `syscall_init()` 原本不在真实启动路径中，导致测量启动 token 为零。已把初始化接入时钟就绪路径，并令宿主拒绝无启动身份的有效窗口；最终两个架构都验证为非零 token。报告增加 provider 后可能挤占屏幕预算，现会减少所展示的日志条目并保留 omitted 数量和完整结束标记；超出不可压缩预算仍明确返回错误。

定时器专项需要 `RAMFS_DIAGNOSTICS=1`，最初对精简镜像调用时，检查器正确报出测试程序不存在；随后使用附带测试程序的独立镜像完成验证。没有把缺少程序当成内核定时器故障，也没有放宽通过条件。

独立组合的证据包状态为 partial，是因为 Boot Report 确实没有编译，文件状态明确为 `not_compiled`；移植和 trace 文件均 available。该组合同时验证关闭日志后仍可拥有独立的诊断启动身份。全部关闭的 ELF 没有日志环、Boot Report、移植/trace 入口、perf 银行、系统调用 profile 表或锁注册表；保留基本 printf/panic、能力描述及原有 ABI 软件事件计数。

真实硬件尚未运行；这些结果不能证明任一实板的 DMA、中断、缓存一致性或性能正常。轨迹的跨 CPU 时间排序没有保证，耗时是包含阻塞和抢占的 elapsed 时间。当前也没有 PMU/完整 CPU 栈采样或崩溃后持久 trace。优化结论需要在目标板上执行同一功能负载和重复基线测量。
