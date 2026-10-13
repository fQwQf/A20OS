# 共享原生内核的真实磁盘执行

本阶段在系统自带 QEMU 10.0.13 上，将已有设备 owner 调用接入驱动 MMIO
接口。两个独立、各一个 vCPU 的 QEMU 共享同一 RAM 文件和 A20OS 内核。
只有主实例挂载 virtio-blk 磁盘；次实例既没有磁盘，也没有 UART。
CPU 1 上的内核线程及 Linux 用户程序均已完成真实磁盘读、写、flush、回读。
这进一步打通 JVM/Minecraft 所需的 I/O 路径，尚未证明客户端跨实例启动。

## 接口与执行路径

`kernel/include/core/shared_mmio.h` 提供同一共享地址域内的接口：

```c
uint64_t shared_mmio_read(unsigned owner, const volatile void *address, unsigned width);
void shared_mmio_write(unsigned owner, volatile void *address, unsigned width, uint64_t value);
```

owner 是逻辑 CPU 端点，width 为 1/2/4/8 字节。地址必须自然对齐、在 owner
上具有相同的内核映射，设备本身必须支持该访问宽度。调用通过
[56](56-shared-device-owner.md) 的同步请求槽发布；请求结构位于调用者内核栈，
完成前调用者保持固定 CPU，栈和地址一直有效。owner 回调只执行原始 volatile
寄存器读写，不拿锁、不等待完成、不分配内存，也不再次调用驱动 HAL。
调用失败或参数不合法时停止内核，避免丢弃写入后仍继续运行。

`driver_hwapi.h` 的 readb/readw/readl/readq、对应写入及 relaxed 32 位访问，
仅在配置 `CONFIG_SHARED_MMIO_OWNER` 时调用该入口。现有驱动函数签名、
队列锁、DMA 缓冲区、完成中断和等待者均保留；调用者原有的读写屏障也保留。
relaxed 版本经请求槽会获得比本地 relaxed 更强的排序，不承诺相同开销。
QEMU 共享平台选择 owner 0；接口本身不依赖 STM32 或 x86 寄存器。
CPU 本地 LAPIC、定时器不走这一驱动 HAL，仍在各自实例操作。

当前验收使用通用 PCI bus 的 virtio transport 和正常可加载 virtio-blk
驱动。远端 CPU 构造 DMA 请求、更新共享队列，队列通知寄存器由 owner 写入；
主实例设备向共享 RAM DMA，并在 CPU 0 触发 MSI-X。CPU 1 使用正常驱动完成
路径继续执行。并未把整个驱动或任意阻塞函数塞进 owner 回调。

## 实测发现与修复

初次磁盘运行出现了真实死锁：CPU 1 持有 virtio 驱动锁并等待 owner 写寄存器，
CPU 0 的设备中断处理同时等待该锁。内核编译的 cpu_relax 已能处理 owner
请求，但独立编译的驱动模块遗漏了 `CONFIG_SHARED_DEVICE_CALL`，其锁等待
只执行 pause，无法完成远端请求。

修复使共享构建的可加载驱动同时启用调用处理钩子和 MMIO 路由，并从驱动框架
导出 shared_device_poll/shared_mmio_read/shared_mmio_write。owner 即使在
驱动锁等待中，也能处理不获取该锁的寄存器叶操作，然后让原持锁者前进。

驱动包含内联 CPU/MMIO 代码，不能与普通内核混用输出。因此共享配置使用
`user/build/x86_64-shared-qemu`，普通配置仍用 `user/build/x86_64`。
用户 ELF 本身没有新增集群 ABI；musl 仍使用原 ISA 构建目录。
普通配置不定义这些宏，HAL 仍为原始内联访问，共享实现编译为空。

## 验收与复现

输出目录必须未存在。每次运行保存源码哈希、diff、冻结的内核 ELF、启动 ROM、
QEMU 命令/QMP 状态、共享 RAM 映射、两个实例的独立日志及 report.json。

```sh
# 内核线程在 CPU 1 上执行真实块设备操作。
python3 tools/check-multi-qemu-kernel.py --workload disk \
  --out .kernel-build/cluster/multi-qemu/disk-new --timeout 110

# 普通 Linux 程序：原线程测试、远端控制台、真实磁盘操作和退出码。
python3 tools/check-multi-qemu-kernel.py --workload linux-disk \
  --out .kernel-build/cluster/multi-qemu/linux-disk-new --timeout 110

# 普通单 QEMU 双 vCPU 对照。
python3 tools/check-multi-qemu-kernel.py --mode smp --workload linux-disk \
  --out .kernel-build/cluster/multi-qemu/linux-disk-smp-new --timeout 110
```

检查器创建专用 2 MiB raw 磁盘，仅连接主实例，启用 writeback cache。
测试先检查容量、扇区大小和第 7 扇区的完整签名，再写第 8..39 扇区：
32 次写入、32 次 flush、32 次回读，共 16384 字节。每轮确认仍在 CPU 1。
停止并等待本次所有 QEMU 后，宿主比较整块 2 MiB 磁盘，既检查写入数据，
也检查签名及其他区域未被修改。磁盘保留为证据；512 MiB 临时共享 RAM
仅在进程停止后删除。不会连接现有用户磁盘，不需要 QEMU 源码。

Linux 程序使用 open/lseek/read/write/close 和已有块设备 ioctl。原始块节点
的 fsync 当前不会向驱动发 flush，因此测试明确使用 `A20_BLK_IOCTL_SYNC`，
不把空操作误报为设备持久化验证。这里尚未验收挂载文件系统的 fsync 或
突然断电后的恢复；宿主检查证明写入到达实际镜像。

生产 shared_mmio.c 也纳入四端点并发用例。x86_64 ASan/UBSan、AArch64
及 RISC-V64 QEMU user 每个各通过 80000 对 MMIO 读写，覆盖所有四种宽度、
窄写截断、轮换 owner、IRQ 状态恢复；原 80000 次通用调用和 316 次嵌套
拒绝检查同时保留。这些用例的寄存器是主机内存，证明公共实现跨 ISA 执行，
不能代替对应物理平台的设备、缓存一致性及总线仲裁验收。

本轮证据位于 `.kernel-build/cluster/multi-qemu/`：

- `owner-disk-01`：启动参数未传入，检查器拒绝缺少磁盘执行的运行。
- `owner-disk-02`：保留驱动锁等待死锁与超时日志。
- `owner-disk-03`：修复后内核远端磁盘执行、迁移、回收及宿主镜像比较通过。
- `owner-disk-baseline-01`：普通 SMP 相同内核磁盘测试通过。
- `owner-linux-disk-01`：用户程序 UAPI 头文件编译路径缺失，保留失败。
- `owner-linux-disk-02`：Linux 用户态磁盘通过；128 次迁移、128 次保护异常、
  1024 片计算分配 451/573，计算总和 4332748800，退出码 0。
- `owner-linux-disk-baseline-01`：普通 SMP 用户态磁盘对照通过，计算分配
  514/510，退出码 0；共享与普通运行的整块磁盘 SHA256 相同。
- `owner-mmio-native-01`：原 Native 线程/文件/子进程/动态工作分配回归通过。
- `mmio-unit-{x86,arm,rv}-01`：三 ISA 公共实现并发检查通过。

`disk-owner-evidence.json` 汇总 11 次运行，并重新核对报告、冻结 ELF、日志和
磁盘哈希。普通内核符号表、普通 virtio-blk 模块未包含共享设备符号；共享模块
包含所需的三个导入。`make check-doc-drift` 通过。失败运行仍保留，未计为通过。

## 当前边界与下一步

BRINGUP 门槛仍保留。下阶段需要实际挂载文件系统、从磁盘加载 JVM/类库，
再验证图形、输入及一个真实 Minecraft 客户端的菜单与输入。GPU/framebuffer
直接映射、端口 I/O、PCI 配置空间动态操作及绕过 HAL 的原始寄存器访问，
不能由本次 MMIO 接入自动保证。owner 目前固定为 0，设备分散到多个 owner
仍需平台资源映射与生命周期管理。

公共指针调用只适用于相同 ISA/内核映像/指针 ABI 的共享执行域，不是混合
ISA 的消息协议。跨 ISA 集群继续使用既有程序格式、能力匹配和传输接口；
物理多 MCU 共享域还需要真实共享 RAM、原子操作/仲裁和缓存维护后端。
owner 失效后的请求取消、fencing 和设备恢复也未完成。当前不能宣称多 MCU
Minecraft 已经可用，亦没有普通模式性能基准来支持整体零性能损失的结论。
