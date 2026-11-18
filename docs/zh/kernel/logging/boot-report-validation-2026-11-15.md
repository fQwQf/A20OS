# 日志与 Boot Report 模块化验证（2026-11-15）

本轮把日志核心、文件写入器、报告章节写入器、Boot Report 收集/观察任务和控制台展示拆开，并实现源码级裁剪。架构及扩展方法见 [日志与报告模块化](README.md)，配置见 [Boot Report 配置与屏幕诊断](boot-report.md)。

## 最终结果

| 验证 | 结果 |
| --- | --- |
| `make -j4 check` | 14 个宿主门禁通过 |
| 全量 Python 回归 | 1562 项，2 项跳过，无失败或错误 |
| 脚本驱动宿主夹具 | 78 个通过，常规 C 宿主测试也通过 |
| 实例声明及架构矩阵 | 121 个实例通过 |
| 最终相关宿主回归 | 23 项通过，包含报告、日志读取工具、实例声明和笔记本恢复夹具 |
| procfs 报告开启/关闭 | 各 49 个 Rust 单元测试及各 200,000 步 C/Rust 差分通过 |
| 默认 RISC-V64 | 不编译报告，等待 12 秒无自动输出，procfs 入口不存在 |
| 手动报告实例 | `boot_report=true, report=off`；不自动输出，手动及并发快照成功 |
| `always` 启动参数 | 已编译支持的内核自动打印一次，手动和并发读取成功 |
| `error` 启动参数 | 健康启动不自动打印，手动及并发读取成功 |
| TOML 紧凑实例 | 2000 ms 窗口、500 ms 收集、8 条、关闭上下文实际生效；自动打印一次 |
| 关闭整个日志系统 | 无报告入口、无自动报告；最小控制台和 shell 正常 |
| x86_64 屏幕诊断模式 | 3000 ms、8 条、关闭上下文生效，自动一次、手动及并发读取成功，BusyBox shell 退出后重启通过 |
| 每个 QEMU 来宾 | 32 次外部子进程、文件操作、同步及正常关机通过 |
| 最终 ELF 符号 | RISC-V 六种场景检查报告与日志环符号，与配置一致 |
| 禁用日志宏 | 参数不求值，不链接日志实现也可编译运行 |
| DRM ABI 门禁 | 48 个 ioctl 值及六种结构布局通过；注入结构编译失败时正确失败 |
| 文档导航 | 271 个活动页面，链接及锚点检查通过 |

QEMU 使用私有磁盘镜像；默认入口仍为 `tools/a20 run qemu-riscv64`。报告启用时，RAM 后端明确标为 `ram/volatile`；日志整体关闭时不生成虚构的持久化成功消息。错误触发由宿主生产函数模拟时钟测试，健康 `error` 来宾没有注入硬件故障。

## 裁剪和性能证据

源码选择测试确认：`LOG_BOOT_REPORT=0` 不编译 `kernel/core/report/` 模块，`LOG_PERSISTENT=0` 不编译 `klog_disk.c`，`LOG_ENABLED=0` 不编译日志核心和文件写入器，只链接 `klog_stub.c` 作为最小控制台兼容层。procfs 的名称、lookup、目录及内容生成分支随报告开关一起裁剪。

真实 RISC-V64 RAMFS ELF 检查确认：报告关闭时没有 `boot_report_format`、`report_printf`、`report_sections`、`klog_snapshot_select`；日志整体关闭时还没有 `klog_buf`、`klog_recs`。本次配置的两块日志环分别为 64 KiB、280 KiB，关闭日志后均消失；最终 ELF 总 BSS 约减少 320 KiB，受布局对齐影响，不能直接把所有数组大小相加当作镜像差值。详细数据在 `size-and-symbols.json`，这些数值仅代表本轮配置。

错误观察已改为读取维护中的计数，不反复搜索日志环。快照按记录条数分配，摘要模式不申请完整记录数组。宿主测试验证错误记录被覆盖后计数正确、最大长度消息、优先级、分配失败、容量边界及不回写日志。

报告输出测试覆盖控制台锁竞争及 16 KiB 文本分块，确认每段至多 64 字节，休眠让出 CPU 前已经恢复中断。分块可能增加 framebuffer 呈现次数，没有据此宣称显示吞吐提高。通用章节测试同时使用 libc 和内核自身格式化函数，验证章节失败可继续、缓冲区不足返回错误；避免内核格式化函数返回实际存入字节数时误把截断当成功。

## 回归中保留及修正的行为

网络后端探针继续显式使用 lwIP，普通 hosted 实例仍验证默认后端策略。此前的 Native 线程错误码传递修复和关机测试场景隔离均保留。

本轮扩大回归发现 DRM 结构检查会把夹具编译失败当成可跳过，导致没有真正检查布局也显示通过。现仅提取需要比较的 wire structs 及其 modeinfo 依赖，编译失败计入门禁失败；以编译器故障注入验证拒绝行为，没有改动 DRM 生产结构。

## 复现与证据

```sh
make -j4 check
make -j4 check-boot-report
python3 tools/qemu/check_boot_report.py --x86 --output .kernel-build/smoke/log-modular/guest-final

# 日常启动、手动报告、紧凑屏幕诊断：
tools/a20 run qemu-riscv64
tools/a20 run qemu-riscv64-report
tools/a20 run qemu-riscv64-screen
```

最终证据目录为 `.kernel-build/smoke/log-modular/`：

- `make-check-final.log`：全量宿主门禁。
- `host-final.log`：最终相关宿主回归，含内核格式化实现。
- `guest-final.log`、`guest-final/result.json`、`guest-final/*.log`：七种来宾场景及符号断言。
- `procfs-off.log`、`procfs-on.log` 及对应目录：关闭/开启的 Rust、C/Rust 差分。
- `size-and-symbols.json`、`*-symbols.txt`：启用、默认裁剪、整体禁用日志三种 ELF 的大小和符号。
- `disabled-macros.log`：日志宏裁掉参数求值；`drm-abi.log`、`drm-negative-control.log`：布局门禁及失败注入。
- `summary.json`、`source.sha256`：结果汇总和相关源码摘要。

这些是 QEMU 和宿主验证结果，尚未验证真实板卡的显示、故障输入、物理落盘、断电保留及硬实时延迟。软件分块长度不是底层硬件最坏延迟保证。
