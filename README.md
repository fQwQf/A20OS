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

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](#) [![Architecture](https://img.shields.io/badge/Arch-RISC--V%20%7C%20ARM64%20%7C%20x86__64%20%7C%20LoongArch64%20%7C%20LoongArch32%20%7C%20PPC64LE%20%7C%20ARMv7-orange.svg)](#)
[![Boards](https://img.shields.io/badge/Boards-18-in--tree-blue.svg)](#)

</div>

## 项目简介
A20OS 是一款探索现代操作系统架构边界的高级混合内核 (Hybrid Kernel)，具备长期的演进与迭代计划。

在架构选型上，A20OS 兼具宏内核与微内核的双重优势：调度、MM、VFS 核心、页缓存与 TCP/IP 数据面等性能关键路径留在内核态，保留极速的函数调用性能；同时深度融合微内核理念——面向能力的 Handle、VMO/VMAR 内存容器与 Channel 通信之上，设备驱动（RTC/input/virtio-blk scratch）与文件系统实现（FAT/ext4/ISO9660/NTFS 只读）均可作为可崩溃、可重启的用户态服务运行。

## Linux ABI 兼容性

A20OS 提供一套完整的 Linux 兼容层，让未修改的 musl 程序（以及 Linux 生态工具链）直接运行在内核机制之上：

* **双 ABI 架构**：`abi/linux`（Linux syscall，运行 musl 生态）与 `abi/native`（面向能力、句柄与事件的 A20 原生接口）共享同一套对象/等待/IPC 底座；Linux 侧只是线格式翻译（`syscall_table.def` 当前登记 366 个入口）。
* **实现全部有效 Linux syscall，覆盖比 Linux 更全面**：riscv64 / aarch64 / loongarch64 / x86_64 四条主线架构完整实现各自的 Linux syscall 编号表——`x86_64` 463 槽全表映射、LoongArch 私有 `file_getattr/file_setattr`、以及 `mseal`/`io_uring`/`landlock`/`pidfd` 等现代 syscall，一应俱全；甚至覆盖了 Linux 编号表中的内部占位槽，无任何未实现的空位。
* **核心实现、ABI 薄包装**：调度、MM（映射记录 + 每页元数据）、VFS、页缓存等状态归属内核子系统，ABI 层只做参数翻译——例如 `mseal` 的区间封禁、fileattr 属性模型、`utime`/`utimes` 时间戳写入都落在核心层。
* **运行真实用户态**：可引导 stock Alpine + XFCE Wayland rootfs，直接运行 git、vim、fastfetch、mksh 等静态链接 musl 程序；`vDSO` 提供 `clock_gettime`/`gettimeofday`/`getcpu` 零陷入快路径。
* **验证门禁**：`make smoke-abi-linux`、`smoke-syscall-ext`、`smoke-mm-stress` 等在 QEMU 中逐组验证 syscall 语义。

> 逐项兼容等级详见 [kernel/abi/linux/syscall_coverage.md](kernel/abi/linux/syscall_coverage.md)。

## 支持的硬件平台
A20OS 具备优秀的跨平台移植性，硬件抽象层 (HAL) 目前官方支持和维护以下目标。
用 `tools/a20 boards` 可以列出树里全部板子及其架构、SMP 与链接脚本情况。

* **虚拟机环境 (QEMU)**：`qemu-virt-riscv64`, `qemu-virt-aarch64`, `qemu-virt-x86_64`, `qemu-virt-loongarch64`、`qemu-virt-ppc64le`（pSeries）、`virtualbox-aarch64`
* **物理开发板**：星光 2 (StarFive VisionFive 2)、龙芯 (Loongson LS2K1000)
* **廉价 SBC / 瘦客户机**（均为 build-verified，尚未上板验证）：
  * [Sipeed LicheeRV Nano](docs/platforms/licheerv-nano.md)（SophGo SG2002，riscv64，256 MiB，约 USD 9–14）
  * [Milk-V Duo](docs/platforms/milk-v-duo.md)（SophGo CV1800B，riscv64，64 MiB，约 USD 10–13）
  * [Allwinner H616/H618](docs/platforms/sun50i-h616.md)（Orange Pi Zero 2 / Zero 3 / Zero 2W，aarch64，约 USD 18–35，**启动链零 blob**）
  * [Rockchip RK3328](docs/platforms/rk3328.md)（Rock64 / NanoPi R2S，aarch64，约 USD 25–35）
  * [通用 PC 兼容机](docs/platforms/x86_64-pc.md)（x86_64 瘦客户机 / N100 迷你主机，约 USD 25–120）
* **MCU bring-up**：STM32F103（ARMv7-M/Cortex-M3，NOMMU；当前提供启动、USART1、SysTick 与基础堆）
  * **不含网络**：`PROFILE=mcu` 走 [components/trim.toml](components/trim.toml) `[profile.mcu].sources` 的独立源文件清单，其中既没有 `kernel/net/*.c` 也没有 lwIP，所以 socket 层与协议栈在这个目标上**从未被编译**，`NET_PROFILE` 对它完全无效。网络对 STM32F103 的支持需要先把网络栈纳入 MCU trim（并按 20 KiB SRAM 重新定档），那是尚未做的产品决定，不是打开某个开关即可。详见 [docs/server-readiness.md](docs/server-readiness.md) §二。

### 关于 NOMMU

`NOMMU_SUPPORTED_ARCHES = riscv64 riscv32 aarch64 arm32 armv7m`（声明在 [components/trim.toml](components/trim.toml) 的能力矩阵，经生成的 `components/trim.mk` 进入构建），无分页路径在这些架构上是真实存在的代码路径（mm/proc/ipc/abi 共 173 处），不是仅能启动的 stub。小内存板（如 Milk-V Duo 的 64 MiB）应当走 NOMMU：

```sh
make ARCH=riscv64 BOARD=milk-v-duo NOMMU=1 RAMFS_USER=1 BRINGUP=1 kernel-only
```

`tools/a20 boards` 与 `make check-arch-boundary`（`smoke-arch-mmu-matrix`）是这条契约的验证入口；`make check-trim-registry` 保证实例校验与构建读到的矩阵不漂移。

* **LoongArch32 (LA32R) bring-up**：`ARCH=loongarch32 BOARD=nailoong` 面向 NaiLoong Core LA32R SoC（软件 TLB refill，无 FPU/IOCSR，单核）；已在 LA32R 全系统模拟器上验证到 `init_kthread`，详见 [docs/platforms/loongarch32.md](docs/platforms/loongarch32.md)

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
[docs/server-readiness.md](docs/server-readiness.md) §二。

## 构建与运行

### 1. 环境准备
需要本机安装各架构交叉工具链（RISC-V/LoongArch/AArch64/x86_64/PPC64LE/ARM）、QEMU 与镜像工具（mtools/dosfstools/e2fsprogs），完整的 apt 命令见 [docs/build.md](docs/build.md) 的"环境准备"一节。

### 2. 编译内核

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
实例字段参考、组件注册表与门禁详见 [docs/instances.md](docs/instances.md)。

其他架构：
```bash
# 构建其他架构 (例: aarch64)
make ARCH=aarch64 BOARD=qemu-virt-aarch64 run

# QEMU pSeries 上运行 PPC64LE（MMU、单核）
make ARCH=ppc64le BOARD=qemu-virt-ppc64le run

# LoongArch32 (LA32R) 内核 bring-up（NaiLoong Core，需 LA32R 工具链，
# 无 QEMU 目标；验证走 cemu 模拟器，见 docs/platforms/loongarch32.md）
make ARCH=loongarch32 BOARD=nailoong BRINGUP=1 kernel-only

# 廉价 SBC / 瘦客户机（build-verified，尚未上板验证）
make ARCH=riscv64 BOARD=licheerv-nano ABI=linux BRINGUP=1 kernel-only
make ARCH=riscv64 BOARD=milk-v-duo    NOMMU=1 RAMFS_USER=1 BRINGUP=1 kernel-only
make ARCH=aarch64 BOARD=sun50i-h616    ABI=linux BRINGUP=1 kernel-only
make ARCH=aarch64 BOARD=rk3328        ABI=linux BRINGUP=1 kernel-only
make ARCH=x86_64  BOARD=x86_64-pc      ABI=linux BRINGUP=1 kernel-only

# STM32F103 64 KiB Flash / 20 KiB SRAM 固件（无网络栈，见上）
make stm32f103-bringup

# 普中玄武 STM32F103ZET6（512 KiB Flash / 64 KiB SRAM）
make stm32f103-xuanwu

# 使用 QEMU STM32VLDISCOVERY（128 KiB Flash / 8 KiB SRAM）验证基础 bring-up
make run-stm32f103-qemu
```

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
├── kernel/platform/  # 板级内存、设备、IRQ、timer 与 SMP 启动
├── docs/             # 设计方案与技术专题说明文档
└── Makefile          # 高度定制化跨平台构建脚本
```

## 许可协议与鸣谢
* 本项目主体代码使用 **Apache 2.0** 协议开源，详见 **[LICENSE](LICENSE)**。
* 第三方组件的集中许可证声明见 **[docs/THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md)**。
* 项目集成、参考和借鉴的第三方项目与公开标准，以及对应的致谢，请参阅 **[docs/ACKNOWLEDGMENTS.md](docs/ACKNOWLEDGMENTS.md)**。
* 镜像文件（`disk.img`、`build/images/<world>-<arch>.img` 等）是构建产物；其实际分发义务取决于镜像内组件、链接方式和精确版本。部分第三方源码是普通 tracked tree，部分是 submodule，不能用单一模式概括；发布前须按 [第三方声明](docs/THIRD_PARTY_NOTICES.md) 逐项核验。
