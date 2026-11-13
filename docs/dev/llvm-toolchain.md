# 使用完整 Clang / LLVM 工具链

A20OS 默认保留 GCC；`TOOLCHAIN=llvm` 选择独立的 LLVM 路径：Clang 集成汇编器、LLD、LLVM binutils 和 compiler-rt builtins。该路径不再调用 GCC / GNU binutils，也不链接 libgcc。GNU Make 保留为构建调度器。

这里的范围是 **A20OS 的编译工具链与目标运行库**。宿主操作系统、Shell 和文件操作工具、QEMU、预编译发行软件及板级第三方固件各有独立依赖；选择 LLVM 不会把整个开发环境或发行镜像变成“无 GPL”。

## 安装

验证环境为 Debian 13，Clang/LLVM 19.1.7。安装 LLVM、Make、CMake、Python 及原有的 QEMU/镜像制作工具：

```sh
sudo apt install clang-19 lld-19 llvm-19 make cmake python3
export PATH=/usr/lib/llvm-19/bin:$PATH
```

不需要安装交叉 GCC 或交叉 binutils。部分 target triple 仍包含 `linux-gnu`，这是目标 ABI 标识；实际编译、链接工具和目标 libc 由这里的 LLVM/musl 配置显式选择。Rust 仍按 `rust-toolchain.toml` 安装；Python、子模块、QEMU 和文件系统工具要求见 [构建指南](build.md)。

fastfetch 使用 Linux UAPI 头文件，并不需要 GNU libc 来链接。按目标架构准备对应 UAPI 头，例如 Debian 的 `linux-libc-dev-arm64-cross`、`linux-libc-dev-armhf-cross`、`linux-libc-dev-riscv64-cross` 和 `linux-libc-dev-loong64-cross`；x86_64 使用宿主的 `linux-libc-dev`。RV32 复用 RISC-V UAPI 头，ABI 类型仍由 RV32 Clang 决定。`FF_LINUX_UAPI_ROOT` 可指定自备的 `<root>/<triple>/include` 目录。现有系统上若已安装交叉 libc 开发包，也能使用其中随附的 UAPI 头，但目标程序链接的是本项目构建的 musl。

## 构建与启动

```sh
# 默认 GCC 路径继续可用
make ARCH=x86_64 BRINGUP=1 NET_STACK=lwip kernel-only

# LLVM：首次构建会准备 compiler-rt，随后编译内核、vDSO 和基础用户态
make TOOLCHAIN=llvm ARCH=x86_64 BRINGUP=1 NET_STACK=lwip -j4 kernel-only
make TOOLCHAIN=llvm ARCH=x86_64 BRINGUP=1 NET_STACK=lwip run

# 单独编译 Linux ABI 用户态
make -C user TOOLCHAIN=llvm ARCH=aarch64 -j4

# 实例入口：通过环境传递工具链选择
TOOLCHAIN=llvm tools/a20 test smoke-quickstart
TOOLCHAIN=llvm tools/a20 build stm32f103-qemu
```

六个主要架构为 `riscv64`、`riscv32`、`aarch64`、`x86_64`、`loongarch64`、`arm32`。ARMv7-M 使用 `armv7m`；STM32 应使用实例入口或显式传入匹配的 MCU profile、Flash/SRAM 和板级配置，不能把通用默认配置当成具体 MCU 的容量配置。已退役的 `loongarch32` 没有 LLVM 映射，选择时会报错；`ppc64le` 保留映射，但不在当前主要验收矩阵中。

### 工具路径

```sh
make TOOLCHAIN=llvm LLVM_PREFIX=/opt/llvm/bin/ ARCH=riscv64 BRINGUP=1 NET_STACK=lwip kernel-only
make TOOLCHAIN=llvm LLVM_SUFFIX=-19 ARCH=x86_64 BRINGUP=1 NET_STACK=lwip kernel-only
```

`LLVM_PREFIX` 是含末尾 `/` 的路径前缀，`LLVM_SUFFIX` 是工具名后缀；`CLANG` 可单独指定编译器可执行文件。建议使用同一版本的 Clang、LLD 和对象工具。工具选择会导出给子 make 及 Python 构建脚本。解包到本地目录的发行版 LLVM 包可能需要设置 `LD_LIBRARY_PATH`；正常系统安装通常不需要。

## compiler-rt 的准备与缓存

`tools/toolchain/build-builtins.py` 从 LLVM 官方 release 下载 compiler-rt 19.1.7 和配套 CMake 模块，验证固定 SHA-256 后解包。首次需要网络；离线构建可预先将下面两个归档放进 `build/toolchains/sources/`：

| 归档 | SHA-256 |
|---|---|
| compiler-rt-19.1.7.src.tar.xz | `c12b6e764202c615c1a3af9a13d477846878757ae0e29e5f8979215a6958fffc` |
| cmake-19.1.7.src.tar.xz | `11c5a28f90053b0c43d0dec3d0ad579347fc277199c005206b963c19aae514e3` |

下载源为 `https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/`。源码许可证保留在解包树中；compiler-rt 使用 Apache-2.0 WITH LLVM-exception，详见源码 `LICENSE.TXT`。CMake 生成 Makefiles，并显式指定 Clang、llvm-ar、llvm-ranlib、llvm-nm 与 LLD；不会用宿主 GCC 构建这些运行库。

运行库输出位于 `build/toolchains/compiler-rt/<arch>-{kernel,user}/builtins.a`，旁边的 JSON 记录编译器、版本、目标、参数和构建脚本标识。RV64 内核的 LP64 与用户态的 LP64D 分开构建；RV32 M-mode MCU 另用 `<arch>-mcu` 的 ILP32 运行库，避免混合软硬浮点 ABI。

可单独准备：

```sh
python3 tools/toolchain/build-builtins.py --arch riscv64 --kind user \
  --output build/toolchains/compiler-rt/riscv64-user/builtins.a
```

不要用宿主运行库代替目标 archive。改变版本、目标或 ABI 后应重新构建；删除对应生成目录可强制从源码重建。

## 各构建入口的行为

| 部分 | LLVM 路径 |
|---|---|
| 内核 C / 汇编 | Clang + 集成汇编器，按架构设置 triple |
| 内核链接 | LLD，所有六个主要架构均不再回退 GNU ld |
| 内核运行库 | compiler-rt builtins；Rust 保留自身的 LLVM 编译路径 |
| vDSO | Clang + LLD + llvm-objcopy |
| musl 与基础 Linux 用户态 | Clang + LLD + LLVM ar/ranlib + compiler-rt；不使用宿主 CRT |
| 通用可加载驱动 | Clang + LLD |
| BSD 网络后端 | Clang；符号隔离、部分链接、符号及重定位检查均使用 LLVM 工具 |
| Native ABI 程序 | Clang + LLD + compiler-rt |
| mlibc RV64 | 生成 LLVM Meson cross file，隔离源码适配和 sysroot，不修改上游子模块 |
| x86_64 / AArch64 UEFI loader | Clang 直接生成 COFF，lld-link 生成 EFI application |
| M0SENSE 探针 / MCU Native 程序 | LLVM 编译、对象工具与运行库入口 |

板级第三方固件项目和可选发行软件不由 `TOOLCHAIN=llvm` 自动移植其上游构建系统。它们不是此处“编译 A20OS 内核与用户态无需 GNU 工具链”的声明范围。

### 关键兼容处理

- LoongArch64 内核使用 medium code model，避免未定义弱函数落在地址 0 时超出短分支范围。
- LLVM 用户态使用 LLD 的 `--image-base=0x10000`，替代 GNU ld 专有的 `-Ttext-segment`。LLD 能重新搜索 archive，无需 ARM 路径中原先的嵌套 group。
- RISC-V 内核显式关闭 PIC/PIE，使 MMU 建立前的 `la` 保持所需的 PC 相对寻址。
- AArch64 内核 C 保持 `-mgeneral-regs-only`；仅汇编允许显式保存和恢复 FP/SIMD 上下文。
- mlibc 的适配源码位于 `build/toolchains/mlibc-source`：静态 rtld 复用 libc 完整 sysdeps，避免重复定义；这个禁用 Linux 选项和测试的交叉构建不探测无用的宿主编译器。
- Clang 额外的 function-type UBSAN 检查由内核诊断处理函数承接；其他现有 sanitizer 检查继续保留。

## 产物隔离

LLVM 内核 variant 包含 `llvm-rt-`，长 variant 可能显示为 `llvm-<hash>`；用户态位于 `user/build/llvm-rt-<arch>`，与 GCC、上一版混合工具链的产物分开。使用下面的命令查询路径：

```sh
make -s TOOLCHAIN=llvm ARCH=x86_64 BRINGUP=1 NET_STACK=lwip print-build-dir
```

musl 缓存记录编译器真实路径、版本、目标参数和构建配置。LLVM 镜像不会从旧 GCC 构建目录拷贝 libgcc_s 或遗留的 musl 共享库。不要手工将不同工具链目录中的对象混合。

## 构建、启动和 GNU 工具调用审计

```sh
python3 tools/check-llvm.py --jobs 4
# Linux 下安装 strace 后，跟踪完整构建进程树；禁用 ccache 命中隐藏编译过程
python3 tools/check-llvm.py --audit --jobs 4
python3 tools/check-llvm.py --audit --strace /path/to/strace --arches x86_64 aarch64
```

脚本固定 `TOOLCHAIN=llvm BRINGUP=1 NET_STACK=lwip`。每个架构构建后，从 Makefile 的 `_qemu_argv` 读取真实 QEMU 命令，默认最多等待 60 秒；正常退出且同时出现 `part ok` 和关机标记才算启动通过。审计会拒绝 GCC/G++、GNU 汇编器、链接器和 binutils（含绝对路径及交叉前缀）；GNU Make 是例外。任何构建、审计或启动失败均返回非零。

日志保存在 `build/llvm-validation/<arch>.log`、`<arch>-boot.log`；审计另有 `<arch>-exec.log`，汇总为 `results.json`。每次汇总只包含本次请求的架构。`--build-only` 跳过启动；`--timeout` 可调整时限。运行前应留出 Makefile 所配置的内存和 CPU 资源。

这证明被测构建路径没有调用 GNU 编译器/binutils；不等价于宿主环境完全不含 GNU 软件，也不证明所有第三方固件都已支持 LLVM。

## 验证范围

2026-11-14，LLVM 19.1.7：六个主要架构均已分别完成完整基础用户态及内核构建、GNU 工具进程审计、QEMU bring-up 自动关机验证。Native handle 测试程序的六架构构建通过。x86_64 UEFI loader 的 OVMF 启动与关机通过；AArch64 UEFI loader 已完成 PE/COFF 构建。其他补充运行验证及日志见本节后续记录。

bring-up 模式不进入用户态，不能用它替代普通 shell、桌面、SMP、NOMMU 或物理板测试。硬件容量也不是工具链兼容性：MCU 必须使用与芯片相符的 Flash/SRAM、裁剪配置。STM32 的结构化日志环按 MCU 单独缩小，避免仅缩小字节环后仍占用数百 KiB SRAM。

补充验证：

- RV64 mlibc 的静态 libc、静态 hello 链接及共享 libc/ld.so 构建通过；Meson 配置过程审计未发现 GNU 编译工具调用。Debian `dpkg-architecture` 的编译器探测也显式使用 Clang。
- STM32F103 QEMU 实例完成 LLVM 构建，并在 `stm32vldiscovery` 上打印初始化成功、进入调度器；这类固件持续运行，验证结束后由测试端停止 QEMU，不要求自动关机。
- CMSIS-RTOS2 RV32 freestanding 对象构建通过。
- 可选 x86_64 `rtw89d` 无线进程的构建与 strip 审计通过，使用 llvm-strip；未据此声明无线硬件运行通过。
- RV32 NOMMU 的 static-PIE init 构建通过；未执行该配置的用户态启动门禁。
- GCC x86_64 内核构建回归通过，默认 GCC 入口保留。

上述补充日志位于 `build/llvm-pure/`。mlibc 源码、交叉文件及构建目录均由 LLVM 专用入口生成，不复用旧 GNU 构建结果。

普通启动验证也已通过：`TOOLCHAIN=llvm tools/a20 test smoke-quickstart` 在默认 BSD 网络后端、both ABI、磁盘根文件系统配置下进入 shell，执行 shell 内建及外部 `/bin/echo`，并正常关机。日志为 `.kernel-build/smoke/smoke-quickstart.log`。

最后一次普通启动复测同时跟踪了整个构建进程树：默认 BSD 后端、主内核和镜像附带的 guest 内核、完整 Native 服务构建、shell 内建/外部命令及关机均通过，审计未发现 GNU 编译工具调用。补充汇总见 `build/llvm-pure/results.json`，原始进程日志为 `quickstart-exec.log`。
