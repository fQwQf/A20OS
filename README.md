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

</div>

## 项目简介
A20OS 是一款探索现代操作系统架构边界的高级混合内核 (Hybrid Kernel)，具备长期的演进与迭代计划。

在架构选型上，A20OS 兼具宏内核与微内核的双重优势：调度、MM、VFS 核心、页缓存与 TCP/IP 数据面等性能关键路径留在内核态，保留极速的函数调用性能；同时深度融合微内核理念——面向能力的 Handle、VMO/VMAR 内存容器与 Channel 通信之上，设备驱动（RTC/input/virtio-blk scratch）与文件系统实现（FAT/ext4/ISO9660/NTFS 只读）均可作为可崩溃、可重启的用户态服务运行。

## Linux ABI 兼容性

A20OS 提供一套完整的 Linux 兼容层，让未修改的 musl 程序（以及 Linux 生态工具链）直接运行在内核机制之上：

* **双 ABI 架构**：`abi/linux`（Linux syscall，运行 musl 生态）与 `abi/native`（面向能力、句柄与事件的 A20 原生接口）共享同一套对象/等待/IPC 底座；Linux 侧只是线格式翻译（`syscall_table.def` 当前登记 361 个入口）。
* **实现全部有效 Linux syscall，覆盖比 Linux 更全面**：riscv64 / aarch64 / loongarch64 / x86_64 四条主线架构完整实现各自的 Linux syscall 编号表——`x86_64` 463 槽全表映射、LoongArch 私有 `file_getattr/file_setattr`、以及 `mseal`/`io_uring`/`landlock`/`pidfd` 等现代 syscall，一应俱全；甚至覆盖了 Linux 编号表中的内部占位槽，无任何未实现的空位。
* **核心实现、ABI 薄包装**：调度、MM/VMA、VFS、页缓存等状态归属内核子系统，ABI 层只做参数翻译——例如 `mseal` 的 VMA 封禁、fileattr 属性模型、`utime`/`utimes` 时间戳写入都落在核心层。
* **运行真实用户态**：可引导 stock Alpine + XFCE Wayland rootfs，直接运行 git、vim、fastfetch、mksh 等静态链接 musl 程序；`vDSO` 提供 `clock_gettime`/`gettimeofday`/`getcpu` 零陷入快路径。
* **验证门禁**：`make smoke-abi-linux`、`smoke-syscall-ext`、`smoke-mm-stress` 等在 QEMU 中逐组验证 syscall 语义。

> 逐项兼容等级详见 [kernel/abi/linux/syscall_coverage.md](kernel/abi/linux/syscall_coverage.md)。

## 支持的硬件平台
A20OS 具备优秀的跨平台移植性，硬件抽象层 (HAL) 目前官方支持和维护以下目标：
* **虚拟机环境 (QEMU)**：`qemu-virt-riscv64`, `qemu-virt-aarch64`, `qemu-virt-x86_64`, `qemu-virt-loongarch64`、`qemu-virt-ppc64le`（pSeries）
* **物理开发板**：星光 2 (StarFive VisionFive 2)、龙芯 (Loongson LS2K1000)
* **MCU bring-up**：STM32F103（ARMv7-M/Cortex-M3，NOMMU；当前提供启动、USART1、SysTick 与基础堆）
* **LoongArch32 (LA32R) bring-up**：`ARCH=loongarch32 BOARD=nailoong` 面向 NaiLoong Core LA32R SoC（软件 TLB refill，无 FPU/IOCSR，单核）；已在 LA32R 全系统模拟器上验证到 `init_kthread`，详见 [docs/platforms/loongarch32.md](docs/platforms/loongarch32.md)

## 构建与运行

### 1. 环境准备
需要本机安装各架构交叉工具链（RISC-V/LoongArch/AArch64/x86_64/PPC64LE/ARM）、QEMU 与镜像工具（mtools/dosfstools/e2fsprogs），完整的 apt 命令见 [docs/build.md](docs/build.md) 的"环境准备"一节。

### 2. 编译内核

根目录不带参数的 `make`/`make all` 是双架构发布构建入口，而不是单架构开发构建。它构建 RISC-V64 与 LoongArch64 的 `PROFILE=benchmark`、8 核、embedded-driver 产物：`kernel-rv`、`kernel-la`、`disk.img`、`disk-la.img`。

日常开发仍使用显式的 `ARCH`、`BOARD` 和 `run`：
```bash
# RISC-V 64 开发镜像并在 QEMU 中运行
make ARCH=riscv64 BOARD=qemu-virt-riscv64 run

# 构建其他架构 (例: aarch64)
make ARCH=aarch64 BOARD=qemu-virt-aarch64 run

# QEMU pSeries 上运行 PPC64LE（MMU、单核）
make ARCH=ppc64le BOARD=qemu-virt-ppc64le run

# LoongArch32 (LA32R) 内核 bring-up（NaiLoong Core，需 LA32R 工具链，
# 无 QEMU 目标；验证走 cemu 模拟器，见 docs/platforms/loongarch32.md）
make ARCH=loongarch32 BOARD=nailoong BRINGUP=1 kernel-only

# 带 GUI 的 QEMU 运行
make run-gui-x86_64
make run-gui-aarch64    # 或 run-gui-arm64
make run-gui-riscv64

# STM32F103 64 KiB Flash / 20 KiB SRAM 固件
make stm32f103-bringup

# 普中玄武 STM32F103ZET6（512 KiB Flash / 64 KiB SRAM）
make stm32f103-xuanwu

# 使用 QEMU STM32VLDISCOVERY（128 KiB Flash / 8 KiB SRAM）验证基础 bring-up
make run-stm32f103-qemu
```

*高级编译选项：*
* `OPT="-O3"` / `OPT="-O0 -g -DDEBUG"`：控制内核优化与调试选项；`MODE` 不控制当前编译参数
* `NR_CPUS=N`：配置 CPU 数；已验证 QEMU SMP 子集可直接使用，其他板必须为明确的 bring-up 实验设置 `ALLOW_UNVERIFIED_SMP=1`

### 3. 编译缓存 (可选)
构建系统对 ccache 提供透明的可选支持：若环境中安装了 [ccache](https://ccache.dev)，内核、用户态、原生测试、驱动包与 extra 包的编译都会自动经过 ccache 加速，重复构建同一参数的目标时显著缩短墙钟时间。
```bash
# 安装 ccache（Debian/Ubuntu）
sudo apt install ccache
```
无需任何配置，检测到即自动生效；未安装时构建行为与之前完全一致，不影响任何功能或门禁。需要临时禁用时可显式传入空值覆盖：
```bash
make CCACHE= ...
```
注意：ccache 只加速宿主机上的本地迭代编译，不影响基准负载在 guest 内对构建耗时的计量。

## 测试与质量保证
作为一个严肃的底层项目，A20OS 建立了一套完备的测试门禁。开发者在提交代码前可通过 `Makefile` 目标进行本地校验：
* **基础 bring-up**：`make smoke-riscv64` 只启动 `BRINGUP=1` 内核，并把 watchdog timeout 视为可接受结果；它不验证系统调用。
* **系统调用测试**：运行 `make smoke-abi-linux`，在 QEMU 中验证 Linux ABI syscall smoke；`smoke-syscall-ext` 覆盖 keyring/AIO/acct/fanotify/pidfd/io_uring/landlock/userfaultfd/perf/fileattr 等扩展 syscall。
* **高负载压力测试**：包含 `smoke-sched-stress`、`smoke-vfs-stress` 等并发压力校验，用于捕获隐蔽的死锁或崩溃。
* **架构合规性验证**：例如 `make check-concurrency-foundation`，在编译期严格审查代码是否符合 SMP 锁模型契约。

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
│   ├── core/         # 核心基础设施 (锁, timekeeping, panic)
│   ├── drivers/      # 混合设备驱动抽象与实现
│   ├── fs/           # 模块化 VFS 框架与各文件系统实现
│   ├── ipc/          # 高级通道通信 (Channels, Events, SysV)
│   ├── mm/           # 内存管理, Native VMO/VMAR, OOM, Page Cache
│   ├── net/          # Socket 层与异步网络进度驱动
│   └── proc/         # 任务调度与状态机
├── kernel/platform/  # 板级内存、设备、IRQ、timer 与 SMP 启动
├── docs/             # 设计方案与技术专题说明文档
└── Makefile          # 高度定制化跨平台构建脚本
```

## 许可协议与鸣谢
* 本项目主体代码使用 **Apache 2.0** 协议开源，详见 **[LICENSE](LICENSE)**。
* 第三方组件的集中许可证声明见 **[docs/THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md)**。
* 项目集成、参考和借鉴的第三方项目与公开标准，以及对应的致谢，请参阅 **[docs/ACKNOWLEDGMENTS.md](docs/ACKNOWLEDGMENTS.md)**。
* 镜像文件（`disk.img`、`extra.img` 等）是构建产物；其实际分发义务取决于镜像内组件、链接方式和精确版本。部分第三方源码是普通 tracked tree，部分是 submodule，不能用单一模式概括；发布前须按 [第三方声明](docs/THIRD_PARTY_NOTICES.md) 逐项核验。
