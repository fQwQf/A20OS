# 共享内核上的 ARMv7-M 原生应用

2026-10-12。此阶段在单内核共享域中增加应用执行路径；跨架构的公共契约见
[27-portable-domains.md](27-portable-domains.md)。ARM 异常入口属于架构适配，
不作为所有平台的集群接口。

## 已实现

可选 `MCU_NATIVE_PROGRAM=1`，当前要求 `MCU_SHARED_KERNEL=1`。默认值 0，
使用独立的 `-program` 构建目录。首版通过内核缓冲区接口
`mcu_program_spawn()` 加载 ELF32 ARM little-endian EABI5 soft-float 静态 PIE。
镜像、应用栈、寄存器状态属于共享堆中的同一个任务；先完成加载与亲和性
设置，再通过现有调度器发布任务。失败加载释放已分配的资源。

接受 Thumb 入口、最多 16 个不重叠 PT_LOAD、最大 256 KiB 镜像跨度、
4–64 KiB 应用栈、最多 256 字节启动数据。启动参数指针通过 r0 传递，
不是 libc 的 argc/argv/auxv 启动约定。支持 PC-relative `-fPIE` 和本地
`R_ARM_RELATIVE`；先验证整个重定位表再应用，拒绝重复目标与覆盖重定位表。
PT_INTERP、PT_TLS、DT_NEEDED、RELA、PLT 导入和其他重定位类型明确拒绝。
示例链接脚本在 `user/tests/mcu-shared-program/link.ld`。

应用在非特权 Thread 模式使用 PSP 执行。`svc #0` 进入真实汇编/C 处理器，
保存 r0–r12、LR、PC、SP 和完整 xPSR。处理器重写异常返回位置，进入
**特权 Thread 模式**的系统调用继续点，再执行通用睡眠/让出/退出逻辑。
因此阻塞不占用 Handler 模式，任务可以沿用普通上下文切换迁到另一个 CPU。
返回使用内部 `svc #0x21` 重建异常帧，保留 IT/NZCV 和栈对齐信息。

本阶段系统调用继续点仍使用同一 PSP 应用栈；没有独立受保护的内核栈切换，
也没有 MPU 地址隔离。用户指针复制只检查所属镜像/栈范围。这是可信应用的
NOMMU 功能原型，非完整的进程内存保护实现。

## 统一的系统调用实现

`kernel/abi/native/sys_thread_basic.c` 同时供服务器与此应用路径使用，包含
原有的 `thread_sleep`、`thread_yield`、`thread_get_cpu`、`thread_exit`。
睡眠走相同 Park/Wake 和 timer heap，get_cpu 走相同 CPU 查询与 copy-out。
`task_exit` 调用现有 `proc_exit_group()`。服务器的处理函数主体没有另造分支。

| Native 编号 | 本阶段行为 |
|---|---|
| `0x0002` execution_info | 公共执行能力描述，见 [28](28-execution-capabilities.md) |
| `0x0200` task_exit | 退出任务组 |
| `0x0206` thread_exit | 退出当前线程 |
| `0x0207` thread_sleep | 定时阻塞，可在别的 CPU 恢复 |
| `0x0208` thread_yield | 现有调度器让出 |
| `0x020e` thread_get_cpu | 写入当前逻辑 CPU ID |
| `0x0b00` / `0x0b01` futex_wait/wake | 公共等待/唤醒，见 [29](29-native-sync.md) |
| `0x0b02` atomic32 | 公共顺序一致原子操作，见 [29](29-native-sync.md) |
| 其他 | `-A20_ERR_NOT_SUPPORTED` |

ARM32 SDK 目前每个参数仅用一个 32 位寄存器，返回值按 int32 符号扩展。
因此本测试的纳秒睡眠参数限制在 uint32 范围内；不能据此声称完整的 64 位
Native ABI 已支持。完整 ARM32 标量参数封装仍待完成；能力查询已经实现。
futex timeout_ns 通过指针参数结构传递，支持完整 u64，不受这个标量限制。

## 可复现证据

需要 ARM bare-metal 工具链、Unicorn/pyelftools；RTL 模式额外需要 Verilator。

```sh
make PYTHON=/path/to/venv/bin/python check-mcu-program
make PYTHON=/path/to/venv/bin/python check-mcu-program-rtl
```

测试实际编译 SDK 应用 ELF 并嵌入固件；应用检查重定位、BSS、坏输出指针、
未知 syscall、让出、休眠、CPU1→CPU0 恢复及栈/全局数据保留。控制线程只通过
真实亲和性接口改变已阻塞任务的目标 CPU。退出后检查唯一 PID、退出码、
程序资源全部回收、两个任务退出、主 CPU 回到 idle，以及隔离复位被拒绝。
固件另拒绝 8 种坏 ELF，并检查失败加载前后可用堆完全恢复。

`tools/check-mcu-program.py` 执行真实应用、SVC 入口、内核处理函数和调度器
指令。它补充 Unicorn 不支持的 Cortex-M 异常硬件压栈/退栈；从实际 PSP/MSP
读取修改后的帧，不通过 context_restore 抹掉 SVC 的修改，也不改任务状态。
仅模拟 SVC/SysTick，不是完整 NVIC。测试的 tick 注入最多每 1000 轮一次，
不是 MCU 周期计时或性能测量。逐条步进规避现有 Unicorn RAM hook 问题。

RTL 模式下共享数据、异常硬件帧及共享代码字节检查进入实际 FSMC 桥、
仲裁器、RAM RTL；指令引擎使用经过核对的镜像执行。引脚适配逐笔完成事务，
并发在途证据仍由独立三端口总线测试提供。报告在
`.kernel-build/cluster/shared-program/`。这不是物理板运行证据。

## 仍待实现

完整 Native ABI、从文件系统启动、动态库、TLS/libc 启动、线程创建/更高层同步、
JVM、显示/input/runtime 依赖，以及实际多板验证仍未闭合。
没有启动真实 Minecraft Java 客户端；验收继续遵循
[24-single-system-minecraft.md](24-single-system-minecraft.md)。

ELF/异常规则参考 [Arm ELF32 ABI](https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst)
和 [ST Cortex-M3 编程手册](https://www.st.com/resource/en/programming_manual/cd00228163.pdf)。

## 动态内存扩展

Native 程序现可通过同一 SDK 的 vm_alloc/vm_unmap 分配和释放共享堆内存，
并将动态同步字用于公共 futex/atomic32。公共所有权、等待期间的引用固定、
退出回收与验证方法见 [30](30-flat-user-memory.md)。这仍未提供完整 JVM、
文件/图形/输入运行时，也不构成 Minecraft 已启动的证据。
