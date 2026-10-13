# 共享内核中的设备 owner 调用

共享 RAM 不会让两个 QEMU 的串口或 PCI 设备自动成为同一个设备。本次增加
显式启用的通用调用入口 `shared_device_call`，并将双 QEMU 平台的控制台交给
CPU 0 所在实例。次实例不创建串口设备，两个 CPU 的正常 console 接口仍可
使用同一个主实例串口。应用不需要新的 syscall。

## 接口和适用范围

接口位于 `kernel/include/core/shared_device_call.h`：

```c
int shared_device_call(unsigned owner, shared_device_handler_t handler,
                       uint32_t operation, uint64_t argument, uint64_t *result);
void shared_device_poll(void);
```

这是同一共享执行域内部的内核接口：代码、指针 ABI、共享 RAM 与地址映射
必须一致。handler 是受信任的内核函数指针，不是跨架构网络协议或用户传入的
回调。现有异构集群 RPC 格式不变；MCU/ARM/RISC-V 可以使用相同接口，但各自
平台仍须提供原子性保证和实际执行进度入口，不能把本次测试推断为这些板型
已经完成设备联调。

每个调用 CPU 有一个有界请求槽。调用者关中断，保持 CPU 和调用栈稳定，发布
目标 owner、操作、参数、handler，再等待完成。owner 执行 handler 后发布
返回状态和结果，调用者读取后才复用槽。一次调用不会把任务或用户上下文
迁到 owner；它只执行一个底层设备操作。owner 就是当前 CPU 时直接调用。

handler 必须是**有界、无锁、不阻塞的底层操作**。不能分配内存、打印、调度、
等待事件或再次调用设备入口；嵌套调用返回 EDEADLK。无效参数返回 EINVAL，
未上线的远端返回 ENODEV，重复占用当前 CPU 请求槽返回 EBUSY。设备暂时未就绪
可返回 EAGAIN，由外围驱动重试。状态码和结果都会返回调用者。

调用者可保留原来的设备锁。owner 服务不能再获取这把锁，否则远端持锁等待
owner、owner 等待远端锁会死锁。这也是此接口只承接底层操作，而不直接运行
完整 VFS/驱动阻塞函数的原因。

## 发布、复用和执行进度

生产实现位于 `kernel/core/shared_device_call.c`：

- IRQ 屏蔽排除同 CPU 的并发生产者和迁移。发布与完成使用 release/acquire；
  非原子的参数和结果只在槽的对应所有权阶段访问。
- owner 用 CAS 领取 REQUEST 状态，领取后再次核对原子 owner。其他端点可能
  在第一次读取和领取之间完成旧请求，使槽被新请求复用；重新核对避免把
  新请求交给旧 owner 执行。未领到槽的 CPU 不读取 handler 或参数。
- 每 CPU 的 servicing 标志与关中断排除 handler 重入；错误的嵌套调用被拒绝。
- x86 双 QEMU 后端在 LAPIC 定时中断和自旋等待的 `arch_cpu_relax` 中服务请求。
  空闲 owner 能由定时中断取得进度；owner 在等调用者持有的锁时也能服务底层
  操作。handler 不能反过来依赖这条服务路径取得进度。

`CONFIG_SHARED_DEVICE_CALL` 当前随 `QEMU_SHARED_SMP=1` 启用。普通构建不编译
请求队列或 poll 调用，原有 `pause` 和 UART 路径保持。该实现没有对已经上线后
停止响应的 owner 实现超时撤销、重新分配或设备 fencing。不能在请求仍可能
执行时提前复用槽；当前共享域仍按整组启动/停止处理。完整故障接管仍未完成。

## 串口适配

`kernel/arch/x86_64/platform/shared_console.c` 为 CPU 0 提供三个有界操作：
发送一个字节、尝试接收一个字节、检查发送是否已完成。每次只检查状态和
读写必要寄存器，不持有驱动锁或等待硬件。正常 UART 接口在操作返回 EAGAIN
时重试，RX 继续使用现有的共享接收环和锁。

主实例负责初始化实际 16550、接收中断及设备寄存器访问。内核日志、Linux
stdio 和 Native stdout 继续使用原接口；次实例的虚拟 UART 不再被当成同一个
设备。次实例不创建串口是验收条件，不是把输出交给宿主合并的替代方案。

这一步没有接入块设备、GPU、PCI/MMIO 的完整 owner 路由，也没有把任意阻塞
驱动操作变成可从 handler 中执行的操作。后续应在硬件访问层接入同样的底层
执行机制，并保留原来的请求完成/IRQ/等待路径，再验证真实磁盘与图形程序。

## 复现和证据

```sh
# 四端点并发，直接执行相同生产 C 文件。
python3 tools/check-shared-device-call.py --arch x86_64 \
  --out .kernel-build/cluster/multi-qemu/device-x86-run
python3 tools/check-shared-device-call.py --arch aarch64 \
  --out .kernel-build/cluster/multi-qemu/device-arm-run
python3 tools/check-shared-device-call.py --arch riscv64 \
  --out .kernel-build/cluster/multi-qemu/device-rv-run

# 两个系统 QEMU；次实例使用 -serial none，主实例使用 -serial stdio。
python3 tools/check-multi-qemu-kernel.py --workload linux \
  --out .kernel-build/cluster/multi-qemu/console-run

# 普通 SMP 对照，关闭共享设备后端。
python3 tools/check-multi-qemu-kernel.py --workload linux --mode smp \
  --out .kernel-build/cluster/multi-qemu/console-smp-run
```

并发测试使用四个 pthread 模拟 CPU 身份/IRQ 状态；每端 20000 次调用，轮换
owner 并集中竞争 owner 0，检查参数、返回值、执行端、嵌套拒绝及 IRQ 状态
恢复。x86_64 用 ASan/UBSan，AArch64/RISC-V64 用静态 ELF 和 QEMU user 执行。
三者均通过 80000 次调用和 316 次嵌套拒绝；这是公共代码的并发与跨 ISA
执行证据，不是物理设备或真实硬中断仿真。

双 QEMU 用例在 [55](55-linux-threads-shared-kernel.md) 的真实 Linux 线程测试
之后把主线程固定到 CPU 1。它通过普通 stdin/stdout 从主实例读入 `owner-ok`，
再输出 32 个确定性块。检查器只从主实例原始日志判断用户验收，并检查每个块
恰好出现一次；不再合并次实例用户输出。QMP 和 `/proc/<pid>/maps` 仍验证
独立单 vCPU 进程与同一共享 RAM inode。

`owner-console-03` 已通过，QEMU PID 为 350321 / 350320，均已结束；次实例日志
只有宿主停止 QEMU 的消息。CPU 1 的收发通过，同时原 Linux TLS、128 次保护
异常、条件变量超时和 1024 片计算通过，工作分配为 600/424。三 ISA 并发报告
位于 `device-call-{x86_64,aarch64,riscv64}-01`。`owner-console-01` 的初次编译
失败日志和后续成功运行均保留，没有覆盖失败证据。

`owner-console-baseline-01` 普通 SMP 对照、`owner-console-native-01` 原 Native
程序和 `owner-console-kernel-01` 共享内核时钟/迁移/回收检查均通过。普通 SMP
内核符号表不含共享设备调用及串口 owner 实现，确认该后端未进入普通构建。
`device-owner-evidence.json` 核对并汇总了五次运行和三个 ISA 单元测试的哈希。

当前证明了共享原生内核中的用户线程可以使用另一个实例独有的真实虚拟设备。
**实际 JVM/Minecraft 跨实例仍未完成**，下一步重点为磁盘、图形和输入设备
的访问及真实运行时启动。MCU 物理部署仍需对应硬件后端与运行时验收。
