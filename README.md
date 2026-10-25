<div align="center">

```text
                    :%%%%%%%.                                 
                    %%%%%%%*                                  
                   -%%%%%%%                                   
                   %%%%%%%-                                   
                  -%%%%%%%                                    
                  %%%%%%%=                                    
                 *%%%%%%%                                     
                .%%%%%%%:=+                                   
                @%%%%%%% %%                                   
                %%%%%%% +%%+                                  
               #%%%%%%+ %%%%                                  
              :%%%%%%% %%%%%-                                 
              %%%%%%%# %%%%%%    ........  .........          
             =%%%%%%% -%%%%%%-   ######### #########=         
             %%%%%%%=  %%%%%%#   ++*+*+*## #*++++++#=         
            *%%%%%%%   :%%%%%%:         ## #+      #=         
            %%%%%%%.    %%%%%%%         ## #+      #=         
           +%%%%%%%     =%%%%%%         *# #+      #=         
           %%%%%%%:      %%%%%%*  ######## #+      #=         
          %%%%%%%%       %%%%%%% :######## #+      #=         
         :%%%%%%%.        %%%%%%::#=       #+      #=         
         %%%%%%%* %%%%%%%%%%%%%%::#=       #+      #=         
        :%%%%%%% -%%%%%%%%%%%%%%::#=       #+      #=         
        %%%%%%%: %%%%%%%%%%%%%%%::######## #########=         
       :======= .===============. -------- ---------:         
```

# A20OS

**高性能、高兼容性的混合内核操作系统**

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](#) [![Architecture](https://img.shields.io/badge/Arch-RISC--V%20%7C%20ARM64%20%7C%20x86__64%20%7C%20LoongArch64%20%7C%20ARMv7-orange.svg)](#)
[![Boards](https://img.shields.io/badge/Boards-18-in--tree-blue.svg)](#)

</div>

## 项目简介

**1.0 开发方向**：以 Rust 为主要内核实现语言完成核心重写，同时完成现有开发中子系统、
修复缺陷，并通过既有功能对照与性能不回退验收。当前实现仍以 C/汇编为主，外部接口尚未
正式冻结。LoongArch32、PPC64LE 已退出 1.0 支持范围，活跃构建入口已清理；旧源码与验证记录保留追溯。
范围与完成定义见 [1.0 计划](docs/releases/v1.0.md)，比较基线和判定规则见
[功能与性能验收](docs/releases/v1.0-validation.md)。

A20OS 是一款探索现代操作系统架构边界的高级混合内核 (Hybrid Kernel)，具备长期的演进与迭代计划。

在架构选型上，A20OS 兼具宏内核与微内核的双重优势：调度、MM、VFS 核心、页缓存与 TCP/IP 数据面等性能关键路径留在内核态，保留极速的函数调用性能；同时深度融合微内核理念——面向能力的 Handle、VMO/VMAR 内存容器与 Channel 通信之上，设备驱动（RTC/input/virtio-blk scratch）与文件系统实现（FAT/ext4/ISO9660/NTFS 只读）均可作为可崩溃、可重启的用户态服务运行。

集群机制通过同一套身份、会话事务、服务发现和算子协议连接 MCU 与服务器：主机使用
Native channel，STM32 叶子通过专用 UART 和静态缓冲执行相同算子，网关可跨 UDP/UART
转发。独立内核及生产叶子代码的混合仿真已验证可靠调用、成员恢复和 16 片/8 节点词频
作业；物理板联调仍待验证。参见[统一设计](docs/cluster/07-unified-runtime.md)、
[实现证据](docs/status/current/cluster-implementation-status.md)、[部署手册](docs/cluster/10-operator-guide.md)、
[服务开发教程](docs/cluster/11-api-tutorial.md)
、[性能测量口径](docs/cluster/21-performance.md)
、[统一单向 SEND](docs/cluster/22-oneway.md)。

## Linux ABI 兼容性

A20OS 提供一套完整的 Linux 兼容层，让未修改的 musl 程序（以及 Linux 生态工具链）直接运行在内核机制之上：

* **双 ABI 架构**：`abi/linux`（Linux syscall，运行 musl 生态）与 `abi/native`（面向能力、句柄与事件的 A20 原生接口）共享同一套对象/等待/IPC 底座；登记入口以各自 `syscall_table.def` 为准。
* **Linux 编号覆盖与语义覆盖分别验证**：四条主线架构提供 Linux syscall 分派与兼容实现，包括 `mseal`、`io_uring`、`landlock`、`pidfd` 等接口。已登记入口仍可能存在 flag、对象或并发语义限制；编号无空位不能证明完整 Linux 兼容，逐项状态与缺口见覆盖表。
* **核心实现、ABI 薄包装**：调度、MM（映射记录 + 每页元数据）、VFS、页缓存等状态归属内核子系统，ABI 层只做参数翻译——例如 `mseal` 的区间封禁、fileattr 属性模型、`utime`/`utimes` 时间戳写入都落在核心层。
* **运行真实用户态**：可引导 stock Alpine + XFCE Wayland rootfs，直接运行 git、vim、fastfetch、mksh 等静态链接 musl 程序；`vDSO` 提供 `clock_gettime`/`gettimeofday`/`getcpu` 零陷入快路径。
* **验证门禁**：`make smoke-abi-linux`、`smoke-syscall-ext`、`smoke-mm-stress` 等在 QEMU 中逐组验证 syscall 语义。

> 逐项兼容等级详见 [kernel/abi/linux/syscall_coverage.md](kernel/abi/linux/syscall_coverage.md)。

## 硬件与虚拟机平台

各目标的硬件范围、运行模式、依赖、构建和部署方法由
[`docs/platforms/`](docs/platforms/README.md) 中的单平台文档说明。
README 不维护设备清单、价格或统一的“已支持”标签。
发布承诺范围见 [平台范围](docs/releases/v1.0.md#2-平台范围)。

`tools/a20 boards` 查询板级源码，`tools/a20 list` 查询实例，
`tools/a20 show <实例名>` 查询对应动作与资源需求；这些查询不证明真机可用。
NOMMU 是按架构和实例选择的运行模式，不能由内存大小推定某块板已经具备该移植。

## 网络栈的当前边界
按名字读大这份能力是最容易出错的地方，所以把边界写在 README 而不是只留在设计文档里：

* **入站 TCP 默认不通。** `a20.tcpmode` 默认 `fast`，此时 listener 只存在于 socket 层，
  任何入站 SYN 都会被 lwIP 回 RST。服务器必须显式传 `a20.tcpmode=lwip`，端口才真正在
  协议栈上 listen。命令行的优先级高于 `/proc/net/config` 写入口。
* **conntrack + NAT 已实现**，但只覆盖 IPv4：无 ALG、**只跟踪 TCP / UDP / ICMP echo，
  不跟踪任何 ICMP 差错报文**（所以依赖 PMTU 发现的路径在这条路径上不工作）、不分片 NAT。
  ICMP echo 跟踪是本轮补上的（`810e9e431`）：类型归一化进 `src_port`、标识符进 `dst_port`，
  回程匹配只交换地址。**端到端门禁只覆盖 DNAT**：QEMU user-net 自己在宿主侧做 NAT，
  guest 外面没有第二个对端，回程包不存在，SNAT/MASQUERADE 不可能有端到端门禁——它由
  主机侧单元门禁 `test-nat-rewrite` 直接编译并断言出货源码（`kernel/net/netfilter_rewrite.c`）
  覆盖。LRU 淘汰与空闲超时有门禁 `smoke-ct-capacity`，但它经 `ctinject` 造流量，而注入器
  只接受 TCP/UDP，**ICMP 条目的淘汰与超时因此没有门禁**。运行时动词挂在
  `/proc/a20/netfilter`。
* **TCP 选项按档位**：`SACK` / 时间戳 / `CUBIC`（RFC 8312 核心条款）默认与服务器档
  开启、嵌入式档关闭。CUBIC **已经实现 §4.2 的 TCP-friendly 目标**（取 `W_cubic` 与
  `W_est` 中较大者），未做的是 HyStart / TCP-AQ / DCTCP / Prague / ECN，以及 `W_max` 的
  跨 RTT 持久化——lwIP 2.2.x 的 `struct tcp_pcb` 没有 `rtt` 字段，本树以
  `TCP_SLOW_INTERVAL` 里的 `pcb->sa` 近似 RTT，因此 §4.2 只在 `W_max` ≳ 3.25 个报文段
  时才真正起作用。`SO_SNDBUF` / `SO_RCVBUF` 真的生效，**抬高 `SO_SNDBUF` 现在对既有连接
  也立即生效**（`272c80a2f`），UDP / RAW 同样接受并执行（`92b399e5d`），
  但**仍然没有自动调优**：没有 `tcp_wmem`/`tcp_rmem`、没有内存压力反馈。
* **驱动校验和卸载刻意不做**：`/proc/net/stats` 里 `tx_csum_offload` / `rx_csum_offload`
  恒为 `off`，`virtio_net` 也从不协商 `VIRTIO_NET_F_CSUM`。这是设计结果，**不是**"上游
  没有这套机制"：lwIP 2.2.2 有承载它的字段与宏（`netif->chksum_flags`、
  `NETIF_SET_CHECKSUM_CTRL()`），只是被 `LWIP_CHECKSUM_CTRL_PER_NETIF`（默认 0）关着，
  而打开它要和 NAT 的原地校验和修正互相打架。依据见
  [docs/net/checksum-offload.md](docs/net/checksum-offload.md)。
* **锁契约两侧现在都可执行**：`LWIP_ASSERT_CORE_LOCKED()` 接到 `g_lwip_lock` 的持有者
  CPU 上，net 锁一侧有 per-CPU 持锁集合探针（`kernel/net/net_lock_probe.c`，`2dd28758c`），
  逐次取锁检查锁序与嵌套深度。**仍未做**：正向查询原语（"当前 CPU 是否持有 socket X 的
  锁"）与 lane claim 一侧探针——两项都在
  [docs/net/network-lock-contract.md](docs/net/network-lock-contract.md) 文末的未勾选项里；
  探针是 per-CPU 而非 per-task，抓不到"取用顺序与释放顺序不一致"。

逐条见 [docs/net/network-config-design.md](docs/net/network-config-design.md)、
[docs/net/network-lock-contract.md](docs/net/network-lock-contract.md) 与
[docs/status/current/server-readiness.md](docs/status/current/server-readiness.md) §二。

## 构建与运行

### 1. 第一次运行：RISC-V 64 串口系统

首次体验推荐 Linux 上的 RISC-V 64 QEMU 实例。它不需要开发板、图形界面或其他架构的工具链。
当前属于开发版本；桌面、服务器与真机的验证范围见 [状态与缺口](docs/status/README.md)。

```bash
git clone --recurse-submodules https://github.com/fQwQf/A20OS.git
cd A20OS
# 已经普通 clone 的用户补执行：git submodule update --init --recursive
```

先按 [首次运行指南](docs/dev/build.md#首次运行linux--risc-v-64) 安装单架构依赖与 Rust，
或按该页的容器方式准备环境，再执行：

```bash
python3 tools/check_quickstart.py             # 缺工具、Rust target、子模块时给出提示
tools/a20 run qemu-riscv64 -- -j4             # 编译完整用户态并启动，保持默认 both ABI
```

看到 shell 提示符后，输入 `/bin/echo hello` 应输出 `hello`；输入 `poweroff` 正常关机并返回宿主终端。
自动验收可运行 `make -j4 smoke-quickstart`：检查 shell、外部程序输出和 QEMU 正常退出。
该验收覆盖完整开发镜像；`smoke-riscv64` 只覆盖内核 bring-up。

推荐预留 8 GiB 宿主内存、20 GiB 可用磁盘；这是准备预算，不是最低硬件规格。
完整步骤、资源等待说明、退出方式及故障排查见 [构建与运行指南](docs/dev/build.md)。

### 2. 其他构建入口

根目录不带参数的 `make`/`make all` 是双架构发布构建入口，而不是单架构开发构建。它构建 RISC-V64 与 LoongArch64 的 `PROFILE=benchmark`、8 核、embedded-driver 产物：`kernel-rv`、`kernel-la`、`disk.img`、`disk-la.img`。

日常开发仍使用显式的 `ARCH`、`BOARD` 和 `run`：
```bash
# RISC-V 64 开发镜像并在 QEMU 中运行
make ARCH=riscv64 BOARD=qemu-virt-riscv64 run
```

也可以用**实例化入口**：`instances/` 下的 TOML 文件声明完整配置，`tools/a20` 负责校验与执行（下面的 `run-*`/`debug-*`/`smoke-*` make 目标都已是它的薄包装）：
```bash
tools/a20 list                     # 列出所有预定义实例
tools/a20 run qemu-riscv64         # 等价于 make ARCH=riscv64 run
tools/a20 run xfce-x86_64          # 图形桌面实例
tools/a20 debug qemu-riscv64       # GDB 调试实例
tools/a20 test smoke-riscv64       # 冒烟测试实例
```
实例字段参考、组件注册表与门禁详见 [docs/dev/instances.md](docs/dev/instances.md)。

其他平台的构建、固件与介质操作请直接阅读对应单平台文档。

*高级编译选项：*
* `OPT="-O3"` / `OPT="-O0 -g -DDEBUG"`：控制内核优化与调试选项
* `NR_CPUS=N`：配置 CPU 数；已验证 QEMU SMP 子集可直接使用，其他板必须为明确的 bring-up 实验设置 `ALLOW_UNVERIFIED_SMP=1`

### 3. 编译缓存 (可选)
构建系统对 ccache 提供透明的可选支持：若环境中安装了 [ccache](https://ccache.dev)，内核、用户态、原生测试与驱动包的编译都会自动经过 ccache 加速，重复构建同一参数的目标时显著缩短墙钟时间。
```bash
# 安装 ccache（Debian/Ubuntu）
sudo apt install ccache
```
无需任何配置，检测到即自动生效；未安装时构建行为与之前完全一致，不影响任何功能或门禁。需要临时禁用时可显式传入空值覆盖：
```bash
make CCACHE= ...
```
注意：ccache 只加速宿主机上的本地迭代编译，不影响基准负载在 guest 内对构建耗时的计量。

## 包管理与镜像分发

A20OS 采用 apk（Alpine 包格式）作为包管理体系：内核、用户态、驱动与移植软件都由 `packages/recipes/` 下的声明式 recipe 打成标准 apk 包，与 Alpine 上游仓库的包一起，按 `packages/world/` 中的 world 清单组合成镜像，并经 GitHub Actions 完成多架构构建、签名与发布。

```bash
make ARCH=riscv64 image-world PKG_WORLD=base   # 打包 → 建库 → 组镜像，一步到位
```

完整文档：[docs/packaging/](docs/packaging/overview.md)（总览、recipe 参考、world 组装、仓库与签名、CI/CD、迁移路线）。

## 测试与质量保证
作为一个严肃的底层项目，A20OS 建立了一套完备的测试门禁。开发者在提交代码前可通过 `Makefile` 目标进行本地校验：
* **基础 bring-up**：`make smoke-riscv64` 只启动 `BRINGUP=1` 的 kernel-only 镜像，要求串口日志出现 `part ok` 与 `System is going down for power-off NOW`，即内核跑完 bring-up 并主动关机；超时退出码 124 视为失败。它不验证系统调用。
* **系统调用测试**：运行 `make smoke-abi-linux`，在 QEMU 中验证 Linux ABI syscall smoke；`smoke-syscall-ext` 覆盖 keyring/AIO/acct/fanotify/pidfd/io_uring/landlock/userfaultfd/perf/fileattr 等扩展 syscall。
* **高负载压力测试**：包含 `smoke-sched-stress`、`smoke-vfs-stress` 等并发压力校验，用于捕获隐蔽的死锁或崩溃。
* **用户态服务测试**：运行 `make smoke-native-fs-all`，验证 svcmgr 托管的用户态文件系统宿主 ufsd 四种后端（FAT/ext4 读写、ISO9660/NTFS 只读）及 SIGKILL 崩溃恢复；`smoke-native-svc`/`smoke-native-registry` 覆盖监管自愈与按名重绑。
* **架构合规性验证**：例如 `make check-concurrency-foundation`，在编译期严格审查代码是否符合 SMP 锁模型契约。
* **网络门禁**：`make smoke-network-suite` 覆盖 socket 与协议栈；`smoke-net-accept` 覆盖真实 lwIP LISTEN pcb 的入站 accept；`smoke-netfilter-nat` 用 QEMU `hostfwd` 打通 DNAT（宿主 18081 → guest 18082）并由宿主侧探针收到回显；`smoke-ct-capacity` 把 conntrack 表填到 `ct_capacity` 断言 LRU 淘汰的是最久未用的那一条、再用短超时断言回收；`test-nat-rewrite` 在主机侧对 SNAT/MASQUERADE 的地址与端口改写、回程元组做单元断言；`smoke-net-tcp-lanes` 是多 lane 下的 TCP 结构性门禁。

## 参与贡献
我们非常欢迎来自开源社区的代码贡献，共同探索下一代操作系统架构！
* 提交 Pull Request 前，请务必在本地运行相关的验证门禁，确保没有引入新的数据竞争或引发回归错误。
* 欢迎查阅 [a20os-improvement-todo.md](docs/roadmap/a20os-improvement-todo.md)，了解当前系统面临的核心工程瓶颈，寻找您感兴趣的开发切入点。

## 设计文档

有关操作系统设计的完整方案、开发过程中的技术瓶颈、解决思路以及并发模型设计，请参阅：
* [操作系统设计方案文档 (OS-Design.md)](docs/OS-Design.md)

## 目录结构
```text
├── kernel/
│   ├── abi/          # 双重 ABI 接口 (linux / native)
│   ├── arch/         # 指令集机制 (trap, page table, context switch)
│   ├── boot/         # 启动路径与链接脚本
│   ├── core/         # 核心基础设施 (锁, timekeeping, panic)
│   ├── drivers/      # 混合设备驱动抽象与实现
│   ├── drvmod/       # 驱动包 (a20drv) 装载框架与示例
│   ├── ext/          # 内核可编程扩展点 (KEP)
│   ├── fs/           # 模块化 VFS 框架与各文件系统实现
│   ├── include/      # 跨架构共享的公共头文件
│   ├── ipc/          # 高级通道通信 (Channels, Events, SysV)
│   ├── mcu/          # 无 MMU 的 MCU (STM32) bring-up
│   ├── mm/           # 内存管理, Native VMO/VMAR, OOM, Page Cache
│   ├── net/          # Socket 层与异步网络进度驱动
│   ├── proc/         # 任务调度与状态机
│   ├── syscall/      # syscall 分发与追踪
│   ├── vdso/         # 各架构 vDSO 时间快路径
│   └── external/     # vendored 第三方源码；lwIP 已带 A20OS 自有改动，见其 DIVERGENCE.md
├── kernel/arch/*/boards/  # 板级内存、设备、IRQ、timer 与 SMP 启动
├── docs/             # 设计方案与技术专题说明文档
└── Makefile          # 高度定制化跨平台构建脚本
```

## 许可协议与鸣谢
* 本项目主体代码使用 **Apache 2.0** 协议开源，详见 **[LICENSE](LICENSE)**。
* 第三方组件的集中许可证声明见 **[docs/THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md)**。
* 项目集成、参考和借鉴的第三方项目与公开标准，以及对应的致谢，请参阅 **[docs/ACKNOWLEDGMENTS.md](docs/ACKNOWLEDGMENTS.md)**。
* 镜像文件（`disk.img`、`build/images/<world>-<arch>.img` 等）是构建产物；其实际分发义务取决于镜像内组件、链接方式和精确版本。部分第三方源码是普通 tracked tree，部分是 submodule，不能用单一模式概括；发布前须按 [第三方声明](docs/THIRD_PARTY_NOTICES.md) 逐项核验。
