# MCU 与服务器的统一执行信息

## 查询契约

公共定义为 `kernel/include/uapi/a20/execution.h`。Native syscall
`execution_info`（0x0002）接收输出指针和容量，容量至少 64 B；当前只写
64 B。空指针/不可写范围返回 FAULT，容量不足返回 INVALID_ARGUMENT。
SDK 为 `a20_execution_info(&info)`。MCU Native 程序和服务器使用同一个
`kernel/abi/native/sys_execution.c`；板型不进入这个实现。
MCU 普通固件没有用户态时，不会因为增加查询而获得一个不存在的 Native ABI。

所有字段为 u32，固定 64 B，在 32/64 位 ABI 中布局一致。系统调用按本机字节序
复制结构；CW/1 QUERY_EXECUTION=4 将每个字段显式序列化为小端，总回复 96 B。
SDK `a20_cluster_execution_query(endpoint, budget_ms, &info)` 校验完整 CW
关联字段和执行描述，失败时不发布部分输出。endpoint 的节点和 boot_epoch
由现有 connect_bound 契约约束；查询结束后身份过期仍须重新连接。
传输/协议失败后应关闭 endpoint，不能将迟到应答解释为下次查询的结果。

| 字节偏移 | 内容 |
|---|---|
| 0、4 | size=64、version=1 |
| 8、12 | ELF e_machine、指针位宽 32/64 |
| 16、20 | 用户镜像字节序、用户镜像调用 ABI profile |
| 24、28 | Native major、minor |
| 32 | 可用接口位图 |
| 36 | 用户地址模型：FLAT=1、PRIVATE_VM=2 |
| 40 | 特权内核原子机制：NATIVE=1、FABRIC_GATE=2 |
| 44–63 | 保留五个 u32，必须为零 |

接口位为 THREAD_BASIC=1、NATIVE_IMAGE=2、HANDLES=4、FUTEX=8、WIDE_ARGS=16、
ATOMIC32=32、USER_HEAP=64、THREAD_CREATE=128（已知位掩码 255）。MCU 实验 Native profile 发布
BASIC、IMAGE、FUTEX、ATOMIC32、USER_HEAP 和 THREAD_CREATE；
等待/唤醒与原子系统调用见 [29-native-sync.md](29-native-sync.md)。
USER_HEAP 表示支持基本匿名页分配/释放，flat 的整块释放与固定 RW 权限契约见
[30-flat-user-memory.md](30-flat-user-memory.md)。THREAD_CREATE 的真实创建、加入与共享空间契约见
[32-native-threads.md](32-native-threads.md)，仍不表示 TLS、完整 VMAR/VMO
或直接调用特权原子库。
WIDE_ARGS 表示现有 syscall 入口可传递完整 64 位标量，当前只对 64 位 profile
发布。它不表示其他所有 Native 调用已经实现。
FLAT 只描述无独立 VM 的地址模型，不证明多个芯片实际连接了共享 RAM。
FABRIC_GATE 描述特权内核互斥方式，不授权应用直接访问硬件门或 IRQ 控制器。

调用 profile ID：x86-64=1、AArch64=2、RV64 double=3、RV32 double=4、
ARM32 hard=5、Thumb soft=6、LoongArch64 double=7、LoongArch32 soft=8、
PPC64 ELFv2=9。映射由构建层选择，校验器拒绝机器号/位宽与 profile 自相矛盾。
这里描述的是本机用户镜像约定，不能从内核 C 编译选项推断：例如 RV64 内核
使用 lp64，而现有 Native SDK 镜像使用 lp64d。本轮新增已执行证据只覆盖
下述测试，不能把表内 ID 视为九类硬件均已联调。

## 镜像匹配与共享内核边界

`a20_execution_requirement_t` 由待选镜像的发布清单提供 ISA/位宽/端序/
调用 ABI、Native major/最低 minor、必需接口位。公共
`a20_execution_image_matches(info, requirement)` 要求全部匹配且目标发布
NATIVE_IMAGE。未知描述、未知版本/特性/保留字段以及不兼容要求均不匹配。
不能根据刚选中的目标反向生成要求，再把这一结果当成镜像的兼容证据。

这个判断是装载的必要条件。它不读取 ELF，也不代替真实加载器对 ELF flags、
ISA 扩展、段范围、权限、重定位、导入、TLS 和入口状态的检查。当前没有统一的
镜像发布清单工具、远程镜像传送或自动按这份描述派发任意 ELF。PROCESS 仍执行
agent 已预装的 manifest ID；算子任务继续由原 CW QUERY 协商，不能强制它们
匹配某个 ISA，从而破坏异构集群执行相同业务的能力。

描述匹配也不等于能够迁移线程：还需属于同一原生执行域、相同任务表/PID、
上下文格式、镜像/栈地址、共享对象可见性与设备所有权。不同 ISA 不能直接
恢复同一份机器寄存器。本接口没有提供伪造的“可跨架构迁移”开关。

## 可复现验证

```sh
python3 tools/check-cluster-unit.py
make check-execution
make smoke-native-cluster
make check-mcu-program
make check-mcu-program-rtl
```

公共 agent 单元测试覆盖九种描述组合、七个独立镜像不匹配维度、显式 LE 编解码、
全部 0–63 B 截断、非法布局/枚举、每个保留字节、容量不足和旧 agent 不支持查询；既有算子、
进程、事务、成员、作业和 UART/UDP 回归继续执行。
`check-execution` 在 x86-64、AArch64、RV64 宿主指令环境执行同一份上述生产
协议测试，并另跑宿主 ASan/UBSan；这不构成内核开机或物理板证据。
RISC-V Native 集群测试从用户态实际调用新 syscall，再向实际启动的 agent
查询并比较内核返回的描述，拒绝错误位宽，覆盖旧算子/进程、任务期限回收、
jobd 二叉归约和词频作业。MCU 原生应用通过真实 SVC 调用相同处理函数，核对
32 位/Thumb soft/flat/fabric profile，并验证短容量和跨越用户范围的输出拒绝；
随后继续原有同 PID/栈跨 CPU 迁移与正常退出回收。

物理板、非 ARM MCU 多芯片端口、完整 Native ABI/JVM/图形输入和
Minecraft Java 客户端仍待完成；执行描述不会将这些待完成能力自动变成可用。
