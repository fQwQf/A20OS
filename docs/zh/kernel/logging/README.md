# 日志系统使用与扩展

日志负责记录事实，报告负责按需组织事实，展示负责把文本送到控制台。三者分别配置和构建，关闭报告不影响日志；关闭内核日志时不编译日志核心及写入器，也不分配日志环。

## 从哪里开始

本目录集中维护日志、报告、移植诊断、性能分析及验证记录。本文是完整内核日志系统的使用入口，先说明配置、查看和保存，再说明开发接口。MCU 使用精简实现，不支持这里的完整报告和轨迹模块。

| 需要完成的工作 | 入口 |
| --- | --- |
| 查看当前启动的内核日志 | 来宾中运行 `dmesg` |
| 确认运行镜像实际包含哪些诊断功能 | `cat /proc/a20/observability` |
| 查看启动状态和近期错误 | 编译启用后读取 `/proc/a20/boot_report`；[配置与输出解读](boot-report.md) |
| 排查启动阶段、驱动探测和未完成检查 | `/proc/a20/porting_report`；[移植与设备测试](observability.md) |
| 记录次数、耗时、事件顺序及锁争用 | `/proc/a20/perf`、`/proc/a20/trace`、`/proc/a20/lock_contention`；[测量与轨迹说明](observability.md) |
| 从 QEMU 或实体板收集一次完整测试 | 宿主运行 `tools/a20 diagnose`；[证据包与比较](observability.md) |
| 读取磁盘上的历史启动日志 | 宿主运行 `tools/a20log`；[存储位置与故障取证](persistent-logging.md) |
| 核对已执行的验证与适用范围 | [日志/报告验证](boot-report-validation-2026-11-15.md)、[移植/测量验证](observability-validation-2026-11-15.md) |

## 日常查看与保存

例如，在宿主启动默认实例：

```sh
tools/a20 run qemu-riscv64
```

进入 A20OS shell 后执行：

```sh
cat /proc/a20/observability
dmesg
dmesg > /tmp/kernel-log.txt
```

能力描述中的 `log/boot_report/porting/metrics/trace` 为运行镜像实际编译的开关，`1` 表示编译支持，`0` 表示裁剪。它不表示已经开始采集或功能测试通过。例如，默认没有 Boot Report 节点属于预期配置；`metrics=1` 时性能窗口仍默认未活动。读取日志、报告和能力描述都不会自动开始测量。

`/tmp/kernel-log.txt` 是临时副本，重启后丢失。内存日志也是有界的，默认文本环 64 KiB、结构化环 1024 条；较早记录会被覆盖。控制台在争用或警告/错误限流时可以省略显示，记录仍先进入内存环；应结合 `dmesg`、报告和文件日志取证，不能只数屏幕上的行数。

需要查看完整的启动和移植快照时，使用已启用对应模块的实例，例如在宿主运行：

```sh
tools/a20 run qemu-riscv64-diagnostics
```

然后在来宾中读取：

```sh
cat /proc/a20/boot_report
cat /proc/a20/porting_report
```

该实例默认不自动打印 Boot Report，保留手动查看；启动轨迹则已开始记录。进行新的 trace 测量前，先 `freeze` 并保存启动轨迹，再开始新窗口，具体步骤见[设备测试与性能分析](observability.md)。

磁盘或宿主上已保存的 JSONL 日志使用 `a20log` 读取。以下命令在宿主执行，`/path/to/log-dir` 应替换为实际日志目录：

```sh
tools/a20log list --dir /path/to/log-dir
tools/a20log summary --dir /path/to/log-dir
tools/a20log show --dir /path/to/log-dir --level warn
tools/a20log show --dir /path/to/log-dir --src NVME --grep timeout
```

`show` 默认选择最新一次启动，`--boot` 可选指定启动；`--level warn` 展示警告及更高级别。`--src` 按子系统标签筛选，`--grep` 按消息内容筛选，两者组合时需同时匹配。原始磁盘镜像使用 `--image /path/to/image.img` 替代 `--dir`，需要宿主安装 mtools。实际存储可能为 RAM、日志分区、根文件系统或 ESP，以本次启动的 `[LOG]` 位置提示和报告中的存储类型为准。

## 配置日志级别与输出

在实例现有的 `[log]` 表中修改字段，不要重复添加同名表。修改后重新构建或打包，并启动更新后的镜像；已运行的镜像不会因为 TOML 文件改变而立即更新。

```toml
[log]
enabled = true
level = "info"
console = true
persistent = false
boot_report = false
report = "off"
```

这组配置保留内存日志和控制台，裁掉磁盘日志写入器和 Boot Report。移植台账、性能指标及 trace 使用各自的开关，不能用 `enabled=false` 代替它们的配置。

| 字段 | 默认值 | 使用效果 |
| --- | --- | --- |
| `enabled` | `true` | 内核日志核心的编译开关；关闭时保留基本 printf/panic 控制台 |
| `level` | `"info"` | `debug/info/warn/err`，分级日志记录该级别及更高的级别；历史 `printf` 输出不一定经过这个阈值 |
| `console` | `true` | 是否把内核日志同步送到串口/framebuffer；关闭它不等于关闭内存记录 |
| `persistent` | `true` | 文件日志模块的编译开关；实际保留能力仍取决于所选文件系统 |
| `mode` | `"auto"` | `auto/partition/rootfs/ram/esp`；[存储选择规则](persistent-logging.md) |
| `max_bytes` | `8388608` | 单次启动 JSONL 文件上限，范围 65536–268435456 字节；达到上限会标为 `capped` |
| `boot_report/report` | 按策略推导 / `"off"` | 报告编译与自动展示；[完整字段](boot-report.md) |
| `porting_report/metrics/trace` | `false/true/false` | 独立选择移植台账、性能/锁统计、事件轨迹；[配置与裁剪](observability.md) |

例如，排查驱动初始化细节时可临时设 `level="debug"`；日常开发恢复 `info`。只需要较重要的分级日志时选择 `warn` 或 `err`，已经过滤或覆盖的上下文无法由报告补回。性能测量要使用 perf/trace 接口，避免在调度、IRQ 或高频 I/O 循环中大量打印文本。

`tools/a20 check <instance>` 校验配置，`tools/a20 show-vars <instance>` 查看派生的 Make 参数。已有镜像可以使用启动参数覆盖部分运行策略，例如 `a20.log.level=debug a20.log.console=1`；启动参数无法恢复被编译裁掉的模块。报告的覆盖顺序和时间窗口见 [Boot Report 配置](boot-report.md)。

## 模块与构建边界

下表适用于完整内核，诊断模块与普通日志可独立选择。

| 模块 | 实现 | 编译条件 |
| --- | --- | --- |
| 日志核心 | `kernel/core/klog.c` | `LOG_ENABLED=1` |
| 持久写入器 | `kernel/core/klog_disk.c` | 日志启用且 `LOG_PERSISTENT=1`；还需平台具备文件系统支持 |
| 通用有界报告写入器 | `kernel/core/report/report.c` | 至少一个报告模块被选中 |
| Boot Report 收集、章节及观察任务 | `kernel/core/report/boot_report.c` | `LOG_BOOT_REPORT=1` |
| 报告控制台展示 | `kernel/core/report/console.c` | Boot Report 启用 |
| 移植台账与报告 | `kernel/core/diagnostics/porting.c` | `DIAG_BOOT=1` |
| 性能计数器与测量窗口 | `kernel/core/perf.c`、`perf_window.c` | `METRICS_ENABLED=1` |
| 锁争用注册与采样 | `kernel/core/lock_counters.c` | `METRICS_ENABLED=1` |
| 事件轨迹与延迟分布 | `kernel/core/diagnostics/trace.c` | `DIAG_TRACE=1` |
| 最小控制台兼容层 | `kernel/core/klog_stub.c` | `LOG_ENABLED=0`；保留 printf/panic 输出，不记录日志 |

Makefile 从源码列表中排除未选择的模块，不依赖链接器垃圾回收。Boot Report 被裁掉时，C 的生成与目录入口、Rust 的名称表、lookup 和内容计划一并裁掉，稳定的内部节点编号保留以免破坏编号镜像。

```toml
# 裁掉内核日志和各类诊断；基本控制台及独立 ABI 功能仍存在。
[log]
enabled = false
persistent = false
boot_report = false
report = "off"
porting_report = false
metrics = false
trace = false
trace_boot = false
```

```toml
# 保留内存日志，裁掉文件写入器和 Boot Report。
[log]
enabled = true
persistent = false
boot_report = false
report = "off"
```

```toml
# 保留按需报告，启动不自动输出。
[log]
enabled = true
boot_report = true
report = "off"
```

`report=error/always` 在没有显式指定 `boot_report` 时自动选择编译支持；显式 `boot_report=false` 与自动展示冲突时校验失败。普通实例不指定报告字段，默认不编译报告。编译关闭的功能不能靠启动参数重新打开。Loader 日志是独立固件组件，默认跟随 `LOG_ENABLED`，仍可显式设置 `loader`；桌面服务自身的标准输出日志也不属于内核日志环。

运行时 `stop/freeze` 只停止采集并保留结果；TOML 编译开关设为 `false` 后重建，才会裁掉对应实现和存储。锁争用统计在 `metrics=true` 时持续累计，perf 的 `stop` 不停止锁统计；可用前后快照分析增量。

屏幕策略、各时间及条数参数见 [Boot Report 配置](boot-report.md)。报告目前支持完整内核配置，MCU 配置不允许启用。

## 添加日志内容

新代码使用统一事件接口，提供级别和子系统名称，不直接访问日志环、不调用报告代码，也不在驱动或中断里写日志文件：

```c
#include "core/klog.h"

klog_event(KLOG_INFO, "NVME", "controller ready queues=%u\n", queues);
klog_event(KLOG_WARN, "INPUT", "device disconnected id=%u\n", id);
klog_event(KLOG_ERR, "NET", "link setup failed error=%d\n", error);
```

子系统名称最多保存 15 字节；结构化记录消息最多保存 239 字节（含前缀，另留一个 NUL 字节）。较长文本可能在文本环与结构化记录中呈现不同长度，应优先记录设备编号、操作、错误码等关键字段。`level` 低于当前 `klog_level` 时跳过。日志启用时，同一事件进入文本环、结构化环及配置的控制台/文件出口，调用点不需要判断具体存储后端。

日志被编译关闭时，`klog_event` 不格式化、不记录，也不求值参数；`kinfo/kwarn/kerr` 宏同样不求值参数。开启编译但因运行级别过滤时，`klog_event` 的调用参数仍会按 C 语义求值。不要把必须执行的业务操作放进日志参数；昂贵快照应在级别允许时再生成。高频跨度和延迟记录使用 `core/trace.h`，新增移植检查使用 `core/diagnostics.h`，示例和约束见 [扩展与开销](observability.md)。

现有 `printf`、`kinfo/kwarn/kerr` 调用兼容。旧消息中的 `[ERR]`、`FATAL`、`PANIC` 等错误标记在写入记录时归一化为错误级别，避免报告反复搜索消息文本。新代码应直接提供正确级别。`report_context=true` 让所有 facility 的保留记录参与筛选，最终展示仍受条数和文本预算限制，不需要每添加一个驱动就修改 Boot Report 白名单。

## 添加报告或章节

公共接口在 `kernel/include/core/report.h`。章节提供 `render(writer, snapshot)`，使用 `report_printf` 输出。写入器统一处理容量，缓冲区不足返回 `-ENOSPC`，不会把截断文本默认为完整报告。章节失败时输出该章节不可用，其他章节继续生成。

```c
static int render_device(report_writer_t *writer, const void *snapshot) {
    const device_snapshot_t *state = snapshot;
    report_printf(writer, "devices: ready=%u failed=%u\n",
                  state->ready, state->failed);
    return 0;
}

static const report_section_t sections[] = {
    {"device", render_device},
};
/* 在进程上下文中取得设备快照，再调用 report_sections。 */
```

扩展现有报告时增加章节函数及其静态表项。新增独立报告时，提供自己的收集器、章节表和入口，并在 Makefile 的 `REPORT_MODULE_SOURCES` 中按独立开关选择源码。通用写入器只在至少一个报告模块需要时编译。不要把新的硬件专用判断加回日志核心。

收集器负责子系统锁和数据所有权；章节只格式化自己的快照。不要在持有日志锁时调用其他子系统、分配内存或输出控制台。快照应有明确容量和覆盖/省略计数，不以 `kernel_phase=ready` 或 PID 1 镜像准备好推断应用健康。

## 性能与并发

- 错误/警告计数随记录写入与环覆盖维护。自动观察每约 500 ms 只读一次错误计数，检查为 O(1)，不再反复扫描 1024 条消息。
- 报告生成时，日志锁内只读取计数、按数值级别筛选并复制选中记录；排序、内存分配、章节回调和格式化都在锁外。选择仍会遍历有限日志环，并非整个生成过程无锁。
- 记录快照按 `report_max_records` 分配：零条摘要只分配状态，小屏八条比四十八条使用更少临时内存；文本输出缓冲区仍固定为 16 KiB。并发打开独立持有快照文本，关闭后释放。
- 自动控制台输出每段最多 64 字节，段间释放中断并让出 CPU；不再为整份多 KiB 报告持续关闭中断。普通控制台日志可能在段间插入，但按需文件快照保持完整。现有 framebuffer 每次写入会呈现屏幕，分块可能增加重绘次数；这项调整改善连续关中断时间，不保证显示吞吐提高。
- 每段输出仍使用底层 UART/framebuffer，慢串口、显示呈现及硬件故障仍可能带来延迟。64 字节是软件发送上限，不是硬实时中断延迟保证；尚未提供目标硬件延迟基准。
- 报告关闭且不编译时没有报告任务、报告状态或快照内存。日志也关闭时，没有文本/结构化日志环或持久写入任务；最小启动控制台仍存在。

## 验证

```sh
make check
make check-boot-report
make check-observability-host
# 两架构 QEMU、编译裁剪及 ELF 检查：
make -j4 check-observability
```

宿主测试覆盖源码选择、通用章节错误与容量边界、记录覆盖后的计数、报告优先级、按条数分配及控制台分块时中断状态。procfs C/Rust 差分分别验证报告开启和关闭配置。QEMU 覆盖默认裁剪、手动保留、自动策略、整体关闭日志和 x86_64 屏幕诊断；使用 `nm` 检查最终 ELF，确认关闭功能的实现和日志环符号确实不在镜像中。实际结果见 [验证记录](boot-report-validation-2026-11-15.md)。

移植检查、可裁剪的性能/锁统计、事件轨迹及 `tools/a20 diagnose/compare-evidence` 见 [设备测试与性能分析](observability.md)。
