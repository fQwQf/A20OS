# `/proc/cpuinfo` 的在线 CPU 上报

`/proc/cpuinfo` 曾在所有架构上返回同一条固定的 `rv64gc` / `sv39` 记录。四 CPU 的 x86 guest 因此被 Minecraft 显示成 `CPU: 1x`，尽管它的 JVM GC 与 Mesa llvmpipe worker 已经在用全部四个 CPU。

共享渲染器现在对在线掩码做快照，按每个在线逻辑 CPU 输出一条 processor 记录。架构自有字段经 `arch_cpuinfo_fields()` 提供；x86 在 `kernel/arch/x86_64/platform/arch_hooks.c` 里用 CPUID 取 vendor 与 model 字符串。回退路径只报架构名，不臆造 ISA、MMU 模式或硬件拓扑。打开文件的缓冲区按配置的 CPU 容量伸缩；这类记录是生成的，procfs 上报大小为 0。

`cpuinfo_test` 断言 processor ID 互不相同、每个 ID 覆盖调用线程的 affinity，并可选地核对预期 CPU 数。`make smoke-cpuinfo` 在单 CPU 的 RISC-V guest 上运行该用例；四 CPU、抢占式 x86 与 RISC-V 的用例与 `sigchld_group_test` 一起跑。

本改动修正的是 processor 上报本身，不构成游戏帧率偏低的原因结论。
