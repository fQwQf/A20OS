# 移植、设备测试与性能分析

本手册说明如何配置诊断模块、读取移植检查、执行性能测量，以及在 QEMU 和实体板上保存、比较测试证据。日常日志查看和新增日志接口见[日志系统使用与扩展](README.md)。

## 数据职责

日志记录离散事实和错误；性能指标回答次数、耗时与规模；事件轨迹用于重建先后顺序和关联；报告消费这些证据。报告读取不得隐式启动或清零测量。耗时使用明确的单调时钟，跨设备对比保留时钟来源及频率信息。

诊断流程复用 `/proc/a20/perf`、`/proc/a20/lock_contention`、Boot Report、宿主快照差分工具和实体板串口传输。新能力通过独立配置选择，不把平台专用条件重新塞入日志核心。

## 实例配置与裁剪

完整内核支持下列 `[log]` 字段。它们是各模块的编译选择，与本次是否正在采集不同。MCU 不包含这套报告和轨迹实现。

| TOML 字段 | 默认值 | 结果 |
| --- | --- | --- |
| `enabled` | `true` | 结构化日志、内存环和日志 API；`false` 裁掉日志核心，保留 printf/panic 的基本控制台 |
| `persistent` | `true` | 文件落盘模块；`false` 不编译磁盘 writer |
| `boot_report` | 自动显示为 error/always 时 true，否则 false | Boot Report 控制器与 `/proc/a20/boot_report`；关闭时 endpoint 不存在 |
| `report` | `"off"` | `off` 只按需读取；`error/always` 启用有限窗口的屏幕诊断，详见 [Boot Report](boot-report.md) |
| `porting_report` | `false` | 编译启动阶段和检查台账，以及 `/proc/a20/porting_report`；关闭时实现、存储和 endpoint 都不存在 |
| `porting_records` | `128` | 保留 16–1024 个检查项；不覆盖尚未完成的项目，满时计入 dropped |
| `metrics` | `true` | 聚合性能计数器与锁争用统计；perf 窗口默认未活动，锁统计沿用启动起累计。`false` 裁掉计数器银行、窗口实现、系统调用 profile 表和锁争用注册/采样实现 |
| `trace` | `false` | 编译有界事件收集器及 `/proc/a20/trace`；关闭时事件调用参数不求值，实现和 per-CPU 存储不链接 |
| `trace_records` | `128` | 每 CPU 16–1024 条事件；总容量不得超过 4096 条，保证导出有界 |
| `trace_mask` | `63` | 初始类别过滤，取 1–63 的位掩码 |
| `trace_boot` | `false` | 时钟初始化后立即开始轨迹；必须同时 `trace=true` |

`metrics=false` 时 `/proc/a20/perf` 只返回 `metrics_compiled: 0`，控制写入返回不支持；独立的 ABI 软件事件计数不属于本模块，仍可服务已有功能。锁报告返回 `lock_metrics_compiled: 0`；锁结构的固定 ABI 字段保留，但不执行统计更新或分配采样表。日志、移植报告、metrics 和 trace 是独立开关：`enabled=false` 不会替代其他模块的显式选择。所有关闭时不会保留它们的报告缓冲区。

例如，移植时保留启动/驱动历史，进入 shell 后再测量高频事件：

```toml
[log]
enabled = true
persistent = false
boot_report = true
report = "off"
porting_report = true
porting_records = 128
metrics = true
trace = true
trace_records = 128
trace_mask = 3       # 启动阶段 + 驱动探测，避免启动时被高频 IRQ 覆盖
trace_boot = true
```

仓库提供 `qemu-riscv64-diagnostics` 和 `qemu-x86_64-diagnostics` 示例。实例对应的 Make 变量依次为 `LOG_ENABLED/LOG_PERSISTENT/LOG_BOOT_REPORT/LOG_REPORT`、`DIAG_BOOT/DIAG_RECORDS`、`METRICS_ENABLED`、`DIAG_TRACE/DIAG_TRACE_RECORDS/DIAG_TRACE_MASK/DIAG_TRACE_BOOT`。诊断构建与日常构建使用不同的构建目录后缀；直接指定 `BUILD_DIR` 时调用者负责隔离。

完整内核中，`cat /proc/a20/observability` 在日志/诊断的不同开关组合下都可读取，返回运行镜像实际编译的模块开关和容量。这个常量描述不会启动收集或链接报告模块。它用于区分“未编译”“已编译未活动”“活动但无事件”，不能仅凭宿主 TOML 推断实际镜像特性。

## 显式测量窗口

完整内核的 `/proc/a20/perf` 现在是只读快照。读取不会启动、停止或清零计数；控制写入使用完整命令，非法后缀和嵌入 NUL 会报错。

```sh
echo start > /proc/a20/perf
# 执行需要测量的负载，并等待它完成。
echo stop > /proc/a20/perf
cat /proc/a20/perf > /tmp/perf-result.txt
```

`start` 创建新代次并清零该窗口的指标与系统调用统计。活动窗口不能再次 `start` 或 `reset`，返回 `EBUSY`；`stop` 关闭更新入口并等待已进入的短写入完成，之后多次读取保持冻结。`reset` 只允许在停止状态，清零并使旧代次失效。`1/0` 分别作为 `start/stop` 的完整命令别名保留。

新快照使用 schema 2，带启动 token、窗口代次、起止时间、elapsed ticks、ticks/s、有效/冻结状态、指标与系统调用行数，以及完整性尾标记。宿主工具拒绝不同启动、不同代次、不同频率或缺少记录的快照差分。旧快照可继续分析，但不能与新格式混合。

系统调用、fork/destroy/COW 和 VirtIO 块设备等待使用窗口代次；跨窗口的耗时样本不会落入新窗口。采样记录的是累计经过时间，包含阻塞等待，不是 CPU cycles。起始 tick 为零有效；时钟倒退的耗时样本被拒绝并计数。写入入口、停写标志和排空检查使用顺序一致的握手，覆盖弱内存序平台；采样开始时也复核代次，拒绝时间戳读取期间发生重置的样本。停止状态的长调用不再更新冻结数据；新窗口中的拒绝计数用于显示跨代次样本。

为保证停止后的稳定性，启用采集时每次指标更新会增加少量 per-CPU 原子操作和短暂禁止抢占；关闭采集时仍走快速返回。证据包记录采集配置；复现实验时同时归档清单所记录 SHA256 的镜像。使用相同负载重复运行，并另行测量空载与关闭诊断的基线，才能估计采集开销。采集器不会自动执行或伪造基线。

## 移植台账与功能状态

`cat /proc/a20/porting_report` 输出 JSONL schema 1。首行给出启动 ID、内核版本、板型、时钟频率和丢失信息；随后是六个启动阶段和有 ID 的检查记录；最后用 `complete=true` 和记录数量标记完整输出。

- 阶段从时钟可用后开始记录。更早的 reset 阶段标为 `untested`，不会补造耗时；READY 表示到达内核检查点。
- 每次驱动探测记录设备名、驱动名、CPU、状态、错误码和 elapsed ticks。重试是独立记录，`deferred` 表示延迟探测，`unsupported` 表示这次探测不支持。
- 检查状态有 `untested/running/pass/fail/unsupported/degraded/deferred`。`pass` 只说明该项检查成功；驱动 probe 返回成功不代表 DMA、中断或设备业务测试通过。
- 挂住的检查保持 `running`，读取时显示已等待时间。普通日志允许 info 且开启控制台时，开始检查与阶段检查点还会打印 breadcrumb；如果进入 shell 前挂住，宿主仍会保留串口记录。
- 完成项可能被新项覆盖，计入 `overwritten`；所有槽位都在运行时拒绝新增项并增加 `dropped`；重复/过期的结束操作增加 `late_finishes`。没有这些记录不等于没有故障。
- `clock_valid=0` 表示未测量或时钟倒退，不能据此计算有效耗时。

完整设备库存、当前绑定和生命周期仍看 `/proc/a20/driver_lifecycle`。台账描述探测尝试，设备库存描述当前状态；未出现的设备不能据此判为正常。固件在 UART 初始化前失败、IRQ 长时间关闭或 CPU 硬锁死时，软件报告也可能无法执行，需要早期串口、调试器或板载恢复手段。

## 按需轨迹和延迟分布

开始新窗口会清空旧轨迹。若使用上面的 `trace_boot=true` 配置或 diagnostics 示例实例，启动轨迹已在采集；先冻结并保存，再开始负载测量。下面的步骤也适用于尚未开始采集的镜像，此时前序快照只含空统计。

```sh
echo freeze > /proc/a20/trace
cat /proc/a20/trace > /tmp/trace-before.jsonl
echo start 63 > /proc/a20/trace
# 运行并等待测试完成；marker 可把自定义阶段放进同一条轨迹。
echo mark 41 > /proc/a20/trace
echo freeze > /proc/a20/trace
cat /proc/a20/trace > /tmp/trace.jsonl
```

`start [mask]` 创建新代次、清零事件与统计并开始；活动时再次 start/reset 返回 EBUSY。`stop` 和 `freeze` 等价，关闭入口并排空短写入后冻结；`reset` 只允许在停止时清零。读取不启停、不清零、不改变过滤器。`mark N` 要求 user 类别开启，N 为 uint32。非法后缀、数值溢出和嵌入 NUL 被拒绝。

| mask 位 | 类别 | 事件载荷 |
| --- | --- | --- |
| 1 | boot | instant 的 id=新阶段，a=旧阶段 |
| 2 | driver | begin 的 a=移植台账检查 ID；建议同时开启 porting_report，才能映射设备和驱动名 |
| 4 | IRQ | begin 的 a=逻辑 IRQ 号；耗时覆盖该 IRQ 的主/共享 handler |
| 8 | I/O | VFS read/write：begin 的 a=请求字节数，b=`(pid << 1) | write` |
| 16 | sched | 切换点的 a=前一个 PID，b=下一个 PID；不是 CPU 栈采样 |
| 32 | user | 手工 marker 的 id=N |

事件号高字节为类别位，低字节 1/2/3 分别表示 begin/end/instant。begin/end 用全局请求 ID 关联，允许跨 CPU 完成；end 的 a 是返回值的 uint64 表示，b 是本次 elapsed ticks，负返回码按有符号 64 位解码。记录有 per-CPU 序号和时间戳。宿主会输出仍能配对的最长事件片段；这些片段不代表被覆盖历史的全部长尾。

每个 CPU 的环覆盖旧事件时计数 `overwritten`。旧代次的完成样本被拒绝；停止后的事件和损失计数保持不变。每类完成操作另有次数、失败数、elapsed 总量、最大值及 32 桶 log2 分布，即使原始事件被覆盖仍保留这些聚合值。桶 0 覆盖 0–1 ticks，桶 n 覆盖 `[2^n, 2^(n+1)-1]`，末桶饱和到更大的值。没有提供伪精确的 p99；可从桶估计范围。

JSONL 首行给出代次、active、mask、容量、CPU 数、起止 ticks 与 ticks/s；每个 CPU 有保留量和丢失计数，最后有事件总数和完整性尾标记。活动快照是各 CPU 的依次截面；冻结后才有稳定结果。时钟使用架构 timer，**不保证跨 CPU 时间戳全局排序**，时钟倒退样本被拒绝计数。所有耗时包含阻塞与抢占，不能相加后宣称为 CPU 占用；文件 I/O 也包含设备及文件系统等待。

## 采集证据包

先按实例构建，再采集；已有镜像可以直接传给 `--kernel`：

```sh
tools/a20 build qemu-riscv64-diagnostics
# make 同一组实例变量的 print-build-dir 可查询镜像路径，或使用 tools/a20 show-vars。
tools/a20 diagnose qemu-riscv64-diagnostics \
  --kernel /absolute/path/to/kernel.elf \
  --output /tmp/a20-run-01 \
  --workload 'my-functional-test'
```

传入 `--kernel` 时使用独立、无磁盘的 QEMU，512 MiB、单 CPU；该镜像须包含可用的 initfs。省略它时使用实例的既有 QEMU 配置与镜像，不自动重新构建。实板使用已有 `[target].serial/baud`：

```sh
tools/a20 diagnose my-board --serial \
  --output /tmp/board-run-01 --timeout 120 \
  --workload 'my-device-test'
```

它连接当前串口并等待 shell，不刷写、不复位板卡；不同提示符用 `--prompt` 正则指定。输出目录必须尚不存在。每条命令均等待唯一结束帧、新提示符和终端交互安静后，才发送下一条。

采集步骤包括实际编译能力、运行身份、移植报告、Boot Report、设备库存、中断及锁争用前后快照、冻结的启动轨迹、性能窗口、测试输出、冻结的负载轨迹、内存/压力和 dmesg。`--workload` 会停止原有采集，保留启动轨迹，再开始新 metrics/trace 窗口；它会取代当前的测量窗口。省略负载则冻结并导出已有数据。测量包含 shell 和采集控制的开销，短负载尤其要注意；命令自身的业务计数及计时应由测试程序输出。

`manifest.json` 保存实例及配置哈希、提供镜像的哈希（QEMU）、宿主源码提交/脏状态、实际 QEMU 命令或串口设备、全部命令和退出码、文件 SHA256、报告校验和分析结果。实板的运行内核只能从来宾身份信息识别，宿主配置和源码不是运行镜像的构建证明。所有运行都保留原始 `serial.log`；32 MiB 上限导致截断时会明确标记。单条命令输出上限 4 MiB，诊断 procfs 上限 1 MiB，超出返回失败而非伪造完整尾标记。

`complete` 表示采集和校验完成；`partial` 表示有模块未编译、不可用或数据无效（每个文件单独标明）；`failed` 表示负载返回非零；超时、断连、无法到达 shell 为 `incomplete`。缺失模块不等于健康；功能是否通过取决于负载检查本身。命令超时后已有输出、退出状态和错误仍保留；实体板可能仍运行原负载，不会自动发送额外控制命令。

比较同一目标的两次优化试验：

```sh
tools/a20 compare-evidence /tmp/a20-run-01 /tmp/a20-run-02 \
  --output /tmp/a20-comparison.json
```

比较器重新核验性能/负载文件哈希与各次冻结窗口，然后比较各次窗口的增量，按各自频率换算 elapsed 秒。它拒绝失败负载、不同命令/架构/目标配置、变更的指标格式；采集配置变化必须显式 `--allow-config-change`，结果会标注。不同设备应各有自己的基线。输出的变化百分比是指标变化，**不是系统吞吐提升百分比**；重复运行并结合负载自己的计时后再判断优化效果。

## 扩展与开销

锁争用统计在 metrics 编译开启后持续累计，perf 的 stop 不停止它；不用时设 metrics=false 裁剪。锁争用报告使用累计次数、循环 spins 和调用点样本；采集器提供前后增量及热点锁。它们不属于冻结的 perf 窗口，包含读取命令的影响。spins 不是 elapsed 时间；累计最大值不能相减得到窗口最大值。新注册的锁没有前序基线，会单独列出；注册表满的 dropped 也会保留。

新增移植检查使用 `core/diagnostics.h`，在过程开始调用 `a20_diag_begin(category, subject, detail)`，结束调用 `a20_diag_end(id, result, error)`。尚未实施的功能用 `UNTESTED`，明确退化路径用 `DEGRADED`，不要把函数返回或设备枚举成功替代功能测试。开始/结束接口不持锁运行被测过程。

新增事件复用 `core/trace.h` 的 begin/end 或 instant API。热路径只写固定大小的数值记录，不分配内存、不格式化文本、不写磁盘。不要传内核地址，使用稳定编号或请求 ID。只在普通内核/IRQ 上下文调用，NMI 类上下文不支持；不要在收集器自己的锁内重入事件。模块驱动使用同一组构建配置，启用时内核导出这些接口。

新增报告通过 `core/report.h` 的有界 writer/provider 组合，从子系统取得拥有独立内存的快照，释放锁后格式化；JSON 字符串通过 `report_json_string` 转义。采集器拥有数据和锁，报告拥有格式与筛选，避免为了增加一类报告修改日志环。需要采集原有指标时在 `perf.h` 枚举、`perf.c` 名称表增加项，并通过 epoch token 记录跨阻塞的耗时，保留停止边界。

- 编译关闭：不链接对应实现与存储；trace 参数不求值。基本 printf/panic 和独立 ABI 功能仍存在。
- 已编译但未活动：metrics 快速返回，trace 检查开关/过滤器；移植台账仍记录低频启动/驱动事实。
- 活动：metrics 有 per-CPU 原子更新与抢占保护；trace 有短 IRQ 安全 per-CPU 锁和时钟读取。驱动诊断还复制有界名称并输出 breadcrumb。必须测量具体板卡上的影响，当前没有宣称固定百分比开销。
- 存储：移植项容量 128 时约几十 KiB；trace 默认每 CPU 128 个 48 字节事件，另有每类直方图和少量元数据。导出在进程上下文分配快照并复制，复制阶段短暂持 per-CPU 锁，格式化不持锁。大容量会增加内存占用和 IRQ 延迟。

当前覆盖启动阶段、驱动探测、通用驱动 IRQ dispatch、VFS 文件读写和调度切换；架构定时器/IPI、驱动内部异步队列、网络协议各层和具体热点函数不自动拥有详细跨度。需要细分时在对应层新增事件/指标。该实现没有提供完整 CPU 栈采样、PMU、崩溃后持久 trace 或分布式时钟同步；不要用现有报告推导这些结论。

## 实施与验收

以下事项共同构成本轮完成条件，不能仅用日志能打印或某项微测试通过替代。

- [x] 启动和移植诊断：记录阶段及驱动初始化时间、设备与驱动结果，区分通过、失败、未测试、不支持和降级；不把 PID 1 镜像准备好当成用户态测试通过。
- [x] 受控性能测量：显式开始、停止、重置；读取不改变采集状态；记录代次、时钟单位和窗口时间；隔离跨窗口调用，并验证并发停止和重置边界。
- [x] 按需事件轨迹：有界、可过滤、明确覆盖丢失数量，能关联驱动、I/O、中断及调度事件；高频路径不格式化长文本、不写磁盘，未编译时裁掉实现和缓冲区。
- [x] 瓶颈证据：次数、累计耗时与等待耗时可区分，提供有界延迟分布和长尾证据；不能把 elapsed ticks 解释成 CPU cycles 或给没有样本的路径输出健康结论。
- [x] 统一宿主工作流：tools/a20 为 QEMU 与实体板卡收集日志、诊断、性能及轨迹；保留失败/超时的部分证据，生成含内核/配置/设备/命令和数据完整性标记的清单。
- [x] 配置与说明：使用 tools/a20 实例字段，记录默认值、开启/关闭结果、采集开销及扩展方法；区分不编译、编译但未采集、已采集但没观察到事件。
- [x] 验证：宿主真实实现的边界/并发/故障注入、启用/裁剪构建及 ELF 符号、至少两个架构的 QEMU 集成，既有完整宿主回归；真实硬件尚未验证的部分必须明确标识。

## 验证与证据

`make check-observability-host` 运行宿主专项；`make -j4 check-observability` 运行启用/关闭构建、ELF 检查、两个架构的 QEMU 和裁剪后的默认 shell。

宿主测试直接编译生产收集器，覆盖严格命令、过滤、覆盖/满槽、失败分配、截断、旧代次及多线程反复停止/重启。跨架构 QEMU 检查覆盖冻结后重复读取、原始记录与宿主解析、移植阶段、过滤后的 marker/I/O、BusyBox 功能、32 次子进程及正常关机。裁剪构建还检查 ELF 符号与默认 shell。

真实硬件尚未在本轮运行，串口传输使用已有 SerialTransport 并通过宿主适配测试。QEMU 证据不能替代设备 DMA、中断可靠性、缓存一致性或真实性能测试。具体本轮执行结果记录在 [验证记录](observability-validation-2026-11-15.md)。
