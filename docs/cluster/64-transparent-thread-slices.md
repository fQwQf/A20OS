# 普通 pthread 的跨机器执行片（实验功能）

这是独立内核、独立 RAM 之间的真实用户线程执行路径。应用不调用远程页 API，
不需要修改 pthread 调用，也不需要在工作节点安装应用二进制。当前实现只支持
**同构 x86_64 Linux ABI、每台机器一个 CPU、一个 home 进程和一个 worker**。
两端必须使用相同构建及兼容的 x87/SSE CPU 功能。

当前模式按执行片串行交接同一进程：远端执行时，本机同进程的其他线程暂时
不参加调度；系统调用交回 home 后恢复正常调度。它实现线程跨机、相同虚拟
地址和透明缺页，但**同一进程不能同时利用多台机器并行计算，不能宣称提供
多机加速**。无共享内存的显式作业和进程部署仍使用既有 cluster 功能。

## 1. 工作方式与语义

1. `a20ssi` 读取配置，通过 `prctl(A20_PR_CLUSTER_SSI)` 为自己的线程组设置
   策略，然后 `execvp` 普通程序。程序参数、逻辑 PID/TID、文件描述符均留在
   home。主线程保留在 home；符合条件的 pthread 子线程在首次进入用户态及
   后续用户态陷阱返回时自动申请远端执行片。
2. Rust 分配不回绕的执行代次，并发布线程组独占执行权。本地原线程作为
   页服务和系统调用代理；调度器保留其他同组线程的队列引用，但不选择它们。
3. 通过 TCP 传送用户通用寄存器、PC/SP、FS/GS TLS 基址和 FXSAVE 状态。
   不传送内核指针、物理页地址、页表、内核栈或内核线程 ID。
4. worker 创建暂时的用户地址空间。真正的指令/读/写缺页向 home 请求页面，
   映射到原来的虚拟地址，使用原映射的 R/W/X 权限。应用普通指针可以访问
   代码、堆、栈、TLS 和线程共享的进程数据。
5. 系统调用、用户 CPU tick 配额或页面/数据额度到达时结束执行片。当前
   保守地返回所有曾授予写权限的页面，即使页面没有实际写脏。home 暂存返回
   页，验证地址、权限、重复返回、上下文和完整性后才写回原 MM 并释放执行权。
   原进程的 COW 通过现有 MM 写路径处理。
6. 系统调用由 home 原线程执行，因此 futex、mutex/barrier/join、文件、管道、
   socket 和退出沿用 home 的内核对象与身份。worker 不执行这些系统调用。
   信号在 home 的正常边界处理，远端执行片期间可能延迟。

这种串行交接提供合法的单处理器线程交错，普通原子指令无需转换成应用 RPC。
它没有实现支持多机同时写共享地址空间的页撤销、所有权目录和分布式原子协议。
执行片结束后 worker 的临时 MM 被回收；下一片重新按需取页。

## 2. 构建与部署

默认 `CLUSTER_SSI=0`，不链接 SSI Rust 状态机，也不会自动监听或连接网络。
启用：

```sh
make ARCH=x86_64 ABI=both NR_CPUS=1 NOMMU=0 CLUSTER_SSI=1 -j4 dev-build
make -C user ARCH=x86_64 -j4 build/x86_64/a20ssi build/x86_64/ssi_pthread_test
make -s ARCH=x86_64 ABI=both CLUSTER_SSI=1 print-build-dir
```

构建目录与普通内核分离。`ABI=linux` 也支持；其他架构、SMP 和 NOMMU 配置
会在构建时被拒绝。工具作为普通用户程序随用户态构建安装；已有镜像可以把
`user/build/x86_64/a20ssi` 和目标程序部署到 home 的 `/bin`。worker 只需要
工具及配置，不需要目标程序或目标程序的文件内容。

配置 home 与 worker 的独立 IP，例如 `10.0.3.2/24`、`10.0.3.3/24`。
**worker 必须使用真正的 TCP 协议栈监听模式**，启动参数中设置：

```text
a20.tcpmode=lwip a20.dhcp=0 a20.ip=10.0.3.3 a20.netmask=255.255.255.0
```

home 使用对应的 `a20.ip=10.0.3.2`。默认 `tcpmode=fast` 的监听器只在一个
内核内部可达；SSI worker 会拒绝这种配置，串口会打印所需启动参数。

先在 worker 执行（当前配置 syscall 需要 root）：

```sh
/bin/a20ssi /bin/ssi-worker.conf
```

看到 `SSI_WORKER: READY` 后，在 home 执行：

```sh
/bin/a20ssi /bin/ssi-home.conf /bin/ssi_pthread_test
# 同样适用于符合下面限制的普通 pthread 程序：
/bin/a20ssi /bin/ssi-home.conf /bin/my-program argument1 argument2
```

`a20ssi` 不后台化 worker。SIGINT/SIGTERM 请求停止；有活动执行片时配置
停止可能返回 EBUSY，退出后 listener 会在当前执行片回收后停止。home 程序
退出时策略回收。进程组 leader 提前 `pthread_exit` 会取消后续自动调度，存活
线程继续在 home 执行。初版每台内核只接纳一个策略所有者；启用时要求
调用者是使用独立 MM 的普通调度策略单线程 leader，已有线程或其他 MM
引用时会拒绝。launcher 在 exec 目标程序之前完成启用。

## 3. 配置文件

UTF-8/ASCII 文本，每行 `key value`，允许空行和 `#` 注释。总长最多 4096
字节，单行必须短于 255 字节。未知字段、重复字段、额外参数、负数或整数
溢出均拒绝，不进行静默修正。`version` 和 `role` 必填；home/worker 还必须
明确填写 `address` 和 `token`。其他项有默认值：

| 字段 | 默认值及范围 | 意义 |
| --- | --- | --- |
| `version` | 必填，`1` | 配置 ABI 版本 |
| `role` | 必填，`off`/`home`/`worker` | 本地执行、进程策略、远端执行器 |
| `shared-memory` | `0`，0/1 | 是否允许透明的同地址空间跨机访问 |
| `automatic-threads` | `0`，0/1 | 是否自动选择 pthread 子线程 |
| `max-pages` | `32`，1..64 | 每片最多映射的页面数，每页 4096 字节 |
| `max-threads` | `4`，1..16 | home 同时记录的子线程额度，退出释放槽位；不是多机并行数 |
| `max-slices` | `64`，1..100000 | 每个子线程生命周期最多远端执行片数 |
| `quantum-ticks` | `2`，1..100 | 每片最多 worker 用户态调度 tick；不计缺页网络等待 |
| `timeout-ms` | `10000`，100..60000 | 每端每片的绝对总期限，包含网络和写回 |
| `max-bytes` | `524288`，8192..1048576 | 每片页数据额度；读页计 4096，可写页预留读回合计 8192 |
| `address` | home/worker 必填，IPv4 | home 的对端地址，worker 的绑定地址 |
| `port` | `47220`，1..65535 | TCP 端口 |
| `token` | home/worker 必填 | 64 个小写十六进制字符，32 字节非零共享口令 |

`max-bytes` 不包含协议头及寄存器上下文，不是每秒速率限制；页面数还会
间接限制页表和元数据分配。home 另预留 `max-pages * 4096` 字节写回暂存，
worker 使用私有页和临时任务/MM。worker 一次只执行一片；它的 pages、quantum、
bytes 上限必须大于等于 home 请求，否则拒绝整片，不静默放宽。两端期限独立。
网络断开与远端页访问失败均不会自动重试执行片。
这些是 SSI 执行器的额外额度，不等于 Linux 的全局资源池或 RLIMIT 统计。
`getcpu`、亲和性和 `/proc` 仍呈现 home 视图；worker 用户 CPU ticks 尚未
合并进 home 的进程 CPU 时间，远端页与内核写回暂存也没有统一计入其 RSS。

任一 home 开关为 0 时，launcher 直接执行普通程序，不提交 SSI 策略，不
发现节点、不联网，在 `CLUSTER_SSI=0` 内核上也能使用此本地模式。worker
要求两个开关都为 1。页面或数据额度不足时，已完成的远端状态交回，当前
缺页在 home 重试；片数或线程槽位不足时线程留在 home。因此可以用很小的
额度限制远端占用，但这会频繁交接且可能基本没有远端计算收益。

完整样例见 `configs/ssi-home.conf`、`configs/ssi-worker.conf` 和
`configs/ssi-off.conf`。示例口令仅供测试；部署时自行生成并在两端使用相同值。

## 4. 支持边界和故障处理

这是受控、互信网络中的实验执行器。口令没有加密、MAC 或防窃听能力；它
不能替代认证的加密传输，worker 执行的是远端提供的用户指令。不要把端口
暴露给不可信网络。当前不做动态成员发现、跨 ISA/CPU 功能翻译或自动接管。

支持普通私有匿名/文件映射，以及 pthread 共享的同一 MM。含 MAP_SHARED、
共享文件、SysV SHM、VMO 或设备 PFN 映射的地址空间会保守留在 home。
受管 exec 不发布 vDSO/vvar，时钟操作走 home 系统调用。实时调度策略和
不同 TGID 的 CLONE_VM/vfork 不在此模式支持范围，相关操作拒绝。普通 fork
产生的独立 COW 进程不继承自动调度策略。

不支持其他进程通过 ptrace、`/proc/pid/mem`、共享文件、设备 DMA 等异步修改
受管内存；这类工作负载应使用本地模式。process_vm 入口检测到活动执行权时
会拒绝其他任务读取或改写受管 MM；这不覆盖此前已经开始的访问，也不等于
所有调试/异步路径均已实现 fencing。
动态库、扩展栈、rseq、用户异常处理、硬件调试寄存器和用户自行读硬件时钟等
工作负载没有完整兼容性保证。首批验证对象是静态链接的普通 musl pthread 程序。

worker 故障、协议错误、期限到达导致状态未完整返回，或写回失败时，home
终止整个受管线程组（SIGKILL）。远端非法用户访问也按失败关闭处理，尚未
转发为原程序的精确信号/异常。程序不会从不确定状态继续或重放用户执行。
这是终止策略，不是故障恢复；网络分区后不提供跨机取消确认和快速接管。

## 5. 验证与观测

```sh
make check-cluster-ssi
python3 tools/check-cluster-ssi-network.py
# 使用已有匹配内核/镜像，镜像只作只读输入并复制：
python3 tools/check-cluster-ssi-network.py --kernel /path/kernel.elf --image /path/fat32.img
```

网络夹具启动两个独立 QEMU 进程，每个各自 RAM 和磁盘 snapshot；宿主仅转发
Ethernet 帧，不生成计算或页服务响应。检查关闭、禁止共享内存、关闭自动
线程、自动执行、页/字节/线程/片数额度，以及执行后切断网络的失败关闭。
`ssi_pthread_test` 同一个普通 ELF 检查堆/栈、TLS、原子计数、mutex、barrier、
FPU、join；自动模式还必须出现 home/worker 执行记录和网络流量。
输出目录包含输入 SHA-256、启动参数、两机日志、抓包和 `report.json`。

`[SSI] home slice` 记录逻辑 TID、代次、页/数据额度和交回原因；
`[SSI] worker slice` 记录真正执行的 worker 临时任务和用户 CPU ticks。
原因 1 为系统调用，2 为配额交回，3 为非法访问/片失败。这些记录证明远端
执行，不能作为多机并行或加速证据。下一阶段仍需并行页一致性、真正的多
worker 调度、完整异常/信号和资源视图、以及分区 fencing。
