# Boot Report 配置与屏幕诊断

Boot Report 是通用启动诊断快照，可用于 QEMU、台式机、笔记本和其他板卡。普通实例默认裁掉报告模块，不自动打印，也没有 `/proc/a20/boot_report`。需要按需查看时，显式设置 `boot_report=true`（例如启动 `tools/a20 run qemu-riscv64-report`），再在 shell 中读取：

```sh
cat /proc/a20/boot_report
cat /proc/a20/boot_report > /tmp/boot-report.txt
```

每次打开获取一份独立快照，分段读取内容保持稳定；再次打开可看到新记录。读取和自动打印都不会把报告再次写入日志。`/tmp` 和 RAM 日志会在重启后丢失，需要保存证据时应复制到磁盘或宿主机。持久日志导出见[启动日志与故障取证](persistent-logging.md)。

## 使用 tools/a20 实例字段

配置沿用现有 `instances/*.toml`，不引入 YAML 或新的配置入口。在自己的实例中添加或修改 `[log]`；同一实例只能有一个 `[log]` 表：

```toml
[log]
boot_report = true
report = "always"
report_window_ms = 10000
report_settle_ms = 500
report_max_records = 16
report_context = true
```

| TOML 字段 | 默认值 | 有效值及效果 |
| --- | --- | --- |
| `boot_report` | 未指定时仅 `report=error/always` 自动启用 | `false` 不编译报告模块和 procfs 入口；`true` 编译支持，包括手动读取。要求日志启用及完整内核配置，MCU 不支持 |
| `report` | `"off"` | `off` 不自动打印；`error` 只在观察窗口发现错误时打印；`always` 最迟在窗口结束打印，发现错误可提前打印 |
| `report_window_ms` | `10000` | 整数 `0..120000`，从诊断线程开始运行计时；`0` 只检查当时已有的错误，不继续观察 |
| `report_settle_ms` | `500` | 整数 `0..10000`，发现错误后继续收集相关日志的时间；`0` 立即生成快照 |
| `report_max_records` | `48` | 整数 `0..48`，最多展示的诊断记录数；`0` 仅显示状态和计数摘要 |
| `report_context` | `true` | `false` 只选警告和错误；`true` 包含各子系统的所有保留记录，新 facility 无需改白名单 |

字段可分别省略，取各自默认值。条数限制只作用于报告，不改变内存日志环容量或 JSONL 记录。初次选取优先最近的错误，再选警告和普通上下文，最后按时间排序。因此 `report_max_records = 8` 不一定展示日志最后连续的八条。

配置上限为 48 条，最终文本预算为 16 KiB。启用移植等额外章节后，如果空间不足，会进一步减少 `shown`，相应增加 `omitted`，保留完整结束标记；不可压缩的章节仍超出预算时返回错误。`report_max_records` 是展示上限，不能据此假定固定展示条数；更多记录应读取内存或文件日志。

报告会打印实际生效的配置，便于确认镜像和启动参数是否正确：

```text
policy: report=always window_ms=10000 settle_ms=500 max_records=16 context=on
```

检查配置、查看构建参数，再正常启动或打包：

```sh
tools/a20 check my-instance
tools/a20 show my-instance
tools/a20 show-vars my-instance
tools/a20 run my-instance
tools/a20 package my-instance
```

`my-instance` 指已经存在的实例名称，也可替换为 TOML 文件路径。配置修改后要重新构建或打包；单纯修改 TOML 不会更新已经写入设备的镜像。类型错误、越界、未知字段，以及 `report="always"/"error"` 配合显式 `enabled=false` 或`console=false`，都会在实例校验阶段报错。

## 常用配置与预期结果

### 日常启动：安静进入 shell

```toml
[log]
report = "off"
```

未指定 `boot_report` 时，`report=off` 会直接裁掉报告功能。普通日志仍然工作，`/proc/a20/boot_report` 不存在；仓库默认实例 `tools/a20 run qemu-riscv64` 使用此行为。

需要静默启动并保留手动读取时使用：

```toml
[log]
boot_report = true
report = "off"
```

可以直接运行 `tools/a20 run qemu-riscv64-report`。`boot_report=false` 配合 `report=error/always` 会报配置冲突。

### 无可用输入：自动保留一份紧凑诊断

```toml
[log]
boot_report = true
report = "always"
report_window_ms = 2000
report_settle_ms = 500
report_max_records = 8
report_context = false
```

没有错误时约 2 秒打印一次；例如约 1 秒发现错误，则额外收集约 0.5 秒后打印。只显示警告和错误，健康启动可能 `shown=0`，状态和计数仍会显示。可直接使用：

```sh
tools/a20 run qemu-riscv64-screen
```

同一组 `[log]` 字段也可用于 x86_64 真机打包实例或其他支持完整日志的板卡。这里的“屏幕诊断”使用内核现有日志控制台，可输出到串口及已初始化的 framebuffer 控制台；它不负责初始化显示驱动，也不保证整份报告能放进一屏或一直留在屏幕上。日志继续输出时仍可能把报告滚走，可减少条数或延长观察窗口。

### 健康启动保持安静，错误时输出

```toml
[log]
report = "error"
report_window_ms = 30000
report_settle_ms = 1000
report_max_records = 24
```

健康启动观察约 30 秒后退出诊断任务，不打印报告。窗口内发现错误时收集约 1 秒后打印一次并退出。错误以保留日志中的结构化错误级别，或兼容旧日志的`[ERR]`、`FATAL`、`PANIC` 等标记判断；普通警告不会触发 `error` 模式。窗口结束后的新错误仍进入正常日志，但不会再次触发自动报告。

观察任务独立于网络探测和 init 运行，不阻塞启动；它最多输出一次后退出。正常调度下约每 500 ms 检查一次，发现错误后的收集时间可能使总时间超过观察窗口。时间均为近似值，受调度延迟影响。它依赖内核时钟、调度器、内存和日志控制台，无法替代硬件 watchdog 或处理调度器完全停滞。

## 临时覆盖与优先级

优先级从高到低为：**本次启动参数 → 命令行 Make 变量 → 实例字段 → 内核默认值**。 Make 变量改变构建缺省值；启动参数只可覆盖已编译模块的行为。`boot_report` 是编译开关，映射为 `LOG_BOOT_REPORT=0/1`，没有运行时启用入口；裁掉后追加 `a20.log.report=always` 也无法恢复功能：

| TOML 字段（`[log]`） | Make 变量 | 启动参数 |
| --- | --- | --- |
| `report` | `LOG_REPORT` | `a20.log.report` |
| `report_window_ms` | `LOG_REPORT_WINDOW_MS` | `a20.log.report_window_ms` |
| `report_settle_ms` | `LOG_REPORT_SETTLE_MS` | `a20.log.report_settle_ms` |
| `report_max_records` | `LOG_REPORT_MAX_RECORDS` | `a20.log.report_max_records` |
| `report_context` | `LOG_REPORT_CONTEXT` | `a20.log.report_context` |

后两种入口的布尔值用 `0/1`，TOML 使用 `false/true`。例如：

```sh
tools/a20 run qemu-riscv64 -- LOG_REPORT=always LOG_REPORT_WINDOW_MS=2000 LOG_REPORT_MAX_RECORDS=8
tools/a20 package x86_64-laptop-shell -- LOG_REPORT=error LOG_REPORT_WINDOW_MS=30000
```

固件、GRUB 或 QEMU 的内核参数可以传入：

```text
a20.log.report=always a20.log.report_window_ms=3000 a20.log.report_context=0
```

数值参数必须是范围内的完整十进制整数；`-1`、`49` 条、`2000ms`、溢出等无效运行时覆盖会被忽略，保留构建缺省值。每项启动参数应只出现一次。`LOG_ENABLED=0`、`LOG_CONSOLE=0` 或相应的启动参数会抑制自动报告；运行时 `a20.log.report=always` 不能越过这项控制台开关。关闭结构化日志后，按需报告不应作为完整故障证据。

## 如何解读报告

| 输出 | 含义 |
| --- | --- |
| `schema` / `boot` | 格式版本与本次启动随机 ID，便于与宿主导出的日志对应 |
| `kernel_phase` / `handoff` | 内核阶段和固件交接方式；`ready` 不表示应用或网络已就绪 |
| `init: prepared` | PID 1 镜像已准备，不代表 shell、桌面或服务已经正常运行 |
| `log: ram` | 实际打开的是 RAM 文件系统，重启丢失；包括 RAM fallback |
| `log: persistent` | 实际文件位于已知磁盘文件系统；不保证故障硬件、未同步尾部或宿主临时盘上的数据耐久性 |
| `log: unknown` / `none` | 无法确认文件系统的保留能力 / 没有可用日志写入器 |
| `writer` | `ready` 正常；`io-error` 写入/短写/同步出错；`capped` 达上限；`metadata-unavailable` 缺少 CURRENT 元数据；`unavailable` 尚未附加写入器 |
| `retained` / `overwritten` | 当前环保留与已覆盖的记录数量，不是整个启动的完整历史 |
| `shown` / `matching` / `omitted` | 展示条数、符合上下文筛选的总数、因条数或文本预算省略的数量 |
| `warnings` / `errors` | 保留环内的计数，不受显示条数和上下文筛选影响，也不是系统健康判定 |

`mode="ram"` 选择日志存储后端，`report="always"` 选择自动展示策略，两者独立。关闭 `enabled` 会从构建中移除日志核心和写入器，连同日志环；只保留最小启动/panic 控制台兼容层。提高 `level` 到 `warn/err` 会减少进入日志环的记录，报告不能恢复已过滤或覆盖的上下文。`persistent=false` 不等同于关闭报告。

## 验证入口

```sh
make check
make check-boot-report
```

前者运行全量宿主门禁，后者额外验证默认 RISC-V64 启动、按需与并发读取、启动参数覆盖、健康 `error` 模式、TOML 紧凑诊断实例、日志整体裁剪和 x86_64 RAMFS 屏幕诊断。每个来宾均执行 shell、 32 次子进程、文件操作、同步及正常关机，使用独立镜像。实际验证记录见[Boot Report 验证记录](boot-report-validation-2026-11-15.md)。

模块边界、增加日志事件和报告章节的方法、性能开销见 [日志与报告模块化](README.md)。

移植检查、可裁剪的性能/锁统计、事件轨迹及 `tools/a20 diagnose/compare-evidence` 见 [设备测试与性能分析](observability.md)。
