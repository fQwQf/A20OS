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
 
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](#)
[![Architecture](https://img.shields.io/badge/Arch-RISC--V%20%7C%20ARM64%20%7C%20x86__64%20%7C%20LoongArch-orange.svg)](#)

</div>

## 项目简介
A20OS 是一款探索现代操作系统架构边界的高级混合内核 (Hybrid Kernel)。项目具备长期的演进与迭代计划，当前正作为参赛内核参与全国大学生操作系统大赛。

在架构选型上，A20OS 兼具宏内核与微内核的双重优势：从运行空间视角，驱动、网络栈与文件系统均在单一特权空间执行，保留了极速的函数调用性能；而在内核逻辑抽象层面，系统深度融合了微内核理念，引入了面向能力的 Handle、VMO/VMAR 内存容器以及 Channel 通信机制。

## 支持的硬件平台
A20OS 具备优秀的跨平台移植性，硬件抽象层 (HAL) 目前官方支持和维护以下目标：
* **虚拟机环境 (QEMU)**：`qemu-virt-riscv64`, `qemu-virt-aarch64`, `qemu-virt-x86_64`, `qemu-virt-loongarch64`
* **物理开发板**：星光 2 (StarFive VisionFive 2)、龙芯 (Loongson LS2K1000)
* **MCU bring-up**：STM32F103（ARMv7-M/Cortex-M3，NOMMU；当前提供启动、USART1、SysTick 与基础堆）

## 构建与运行

### 1. 环境准备
项目根目录提供了完整的 `Dockerfile`，用于快速构建全架构（RISC-V/x86/ARM/LoongArch）的交叉编译环境。
```bash
# 构建镜像
docker build -t a20os-buildenv .

# 启动容器
docker run -it --rm -v $(pwd):/workspace -w /workspace a20os-buildenv bash
```

### 2. 编译内核
内核支持一键编译与启动，默认架构为 `riscv64`：
```bash
# 默认编译为 RISC-V 64 并在 QEMU 中运行
make ARCH=riscv64 BOARD=qemu-virt-riscv64 run

# 构建其他架构 (例: aarch64)
make ARCH=aarch64 BOARD=qemu-virt-aarch64 run

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
* `MODE=release/debug`: 编译模式（默认 release 包含 O3 优化）
* `NR_CPUS=N`: 开启多核并发模式（需要配置门禁开关）

## 测试与质量保证
作为一个严肃的底层项目，A20OS 建立了一套完备的测试门禁。开发者在提交代码前可通过 `Makefile` 目标进行本地校验：
* **系统调用测试**：运行 `make smoke-riscv64`、`smoke-abi-linux` 等目标，验证内核基础能力。
* **高负载压力测试**：包含 `smoke-sched-stress`、`smoke-vfs-stress` 等并发压力校验，用于捕获隐蔽的死锁或崩溃。
* **架构合规性验证**：例如 `make check-concurrency-foundation`，在编译期严格审查代码是否符合 SMP 锁模型契约。

## 参与贡献
我们非常欢迎来自开源社区的代码贡献，共同探索下一代操作系统架构！
* 提交 Pull Request 前，请务必在本地运行相关的验证门禁，确保没有引入新的数据竞争或引发回归错误。
* 欢迎查阅 [a20os-improvement-todo.md](docs/a20os-improvement-todo.md)，了解当前系统面临的核心工程瓶颈，寻找您感兴趣的开发切入点。

## 赛事与设计文档

有关操作系统设计的完整方案、开发过程中的技术瓶颈、解决思路以及并发模型设计，请参阅：
* [操作系统设计方案文档 (OS-Design.md)](docs/OS-Design.md)
* [初赛汇报幻灯片 (PDF)](docs/slides/main.pdf) | [LaTeX 源文件](docs/slides/main.tex)

## 目录结构
```text
├── kernel/
│   ├── abi/          # 双重 ABI 接口 (linux / native)
│   ├── arch/         # 硬件架构相关的 HAL (aarch64, riscv64, etc.)
│   ├── core/         # 核心基础设施 (锁, timekeeping, panic)
│   ├── drivers/      # 混合设备驱动抽象与实现
│   ├── fs/           # 模块化 VFS 框架与各文件系统实现
│   ├── ipc/          # 高级通道通信 (Channels, Events, SysV)
│   ├── mm/           # VMO/VMAR 内存管理, OOM, Page Cache
│   ├── net/          # Socket 层与异步网络进度驱动
│   └── proc/         # 任务调度与状态机
├── docs/             # 比赛设计方案与技术专题说明文档
├── Dockerfile        # 编译环境容器配置
└── Makefile          # 高度定制化跨平台构建脚本
```

## 许可协议与鸣谢
* 本项目主体代码使用 Apache 2.0 协议开源。
* 内核网络协议栈以 `NO_SYS=1` 的模式集成了轻量级的 [lwIP](https://savannah.nongnu.org/projects/lwip/)，遵循其原始的 BSD 许可证。
* 感谢全国大学生操作系统大赛平台提供的交流机会与测试环境。
