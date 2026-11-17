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

[简体中文](README.md) · [English](README.en.md) · [English documentation](https://fqwqf.github.io/A20OS/docs/en/)

**A high-performance hybrid-kernel operating system with broad compatibility**

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](#) [![Architecture](https://img.shields.io/badge/Arch-RISC--V%20%7C%20ARM64%20%7C%20x86__64%20%7C%20LoongArch64%20%7C%20ARMv7-orange.svg)](#) [![Boards](https://img.shields.io/badge/Boards-18-in--tree-blue.svg)](#)

</div>

## Introduction

A20OS is a high-performance hybrid kernel exploring the boundaries of modern operating system architecture. It combines the performance of a monolithic kernel with microkernel fault tolerance and capability security on a shared foundation:

- **Hybrid kernel architecture**: performance-critical paths, including scheduling, MM, the VFS core, the page cache and the TCP/IP data plane, remain in kernel space for fast function calls. Microkernel mechanisms include capability-based Handles, VMO/VMAR memory containers and Channel communication. Device drivers (RTC/input/virtio-blk) and file system implementations (FAT/ext4/ISO9660/NTFS read-only) can run as isolated user-space services that can crash and restart.
- **Dual ABI ecosystem**: the Linux ABI supports unmodified upstream musl programs and common distribution toolchains; the A20 Native ABI provides capability-based, event-driven interfaces.
- **Multiple platforms and unified clustering**: supported architectures include RISC-V, ARM64, x86_64, LoongArch64 and embedded ARMv7-M / RISC-V 32. Unified node identities and an operator protocol connect low-power MCUs through high-performance servers to a distributed runtime and a shared-memory multi-machine kernel. See the [unified cluster architecture](docs/zh/cluster/README.md).
- **1.0 evolution goals**: gradually introduce Rust into core scheduling and state machines to strengthen memory safety and complex lifetime management, with functional and performance regression checks. See the [1.0 plan](docs/zh/releases/v1.0.md) and [acceptance baseline](docs/zh/releases/v1.0-validation.md).

## Linux ABI compatibility

A20OS provides a comprehensive Linux compatibility layer so that unmodified musl programs and Linux ecosystem toolchains can run on its kernel mechanisms:

* **Dual ABI architecture**: `abi/linux` (Linux syscalls for the musl ecosystem) and `abi/native` (A20 interfaces based on capabilities, handles and events) share the same object, wait and IPC foundation. Each ABI's `syscall_table.def` is authoritative for registered entries.
* **Syscall-number coverage and semantic coverage are validated separately**: the four main architectures provide Linux syscall dispatch and compatibility implementations, including `mseal`, `io_uring`, `landlock` and `pidfd`. Registered entries may still have limitations in flags, objects or concurrency semantics. A fully populated number table does not establish complete Linux compatibility; consult the coverage table for individual status and gaps.
* **Core implementations with thin ABI wrappers**: scheduling, MM (mapping records and per-page metadata), VFS and page-cache state belong to the kernel subsystems. ABI layers only translate arguments. For example, range sealing for `mseal`, the fileattr attribute model and timestamp updates for `utime`/`utimes` reside in the core.
* **Real user-space software**: boot a stock Alpine + XFCE Wayland rootfs and run statically linked musl programs such as git, vim, fastfetch and mksh. The `vDSO` supplies trap-free fast paths for `clock_gettime`/`gettimeofday`/`getcpu`.
* **Validation checks**: `make smoke-abi-linux`, `smoke-syscall-ext` and `smoke-mm-stress` validate syscall semantics in groups under QEMU.

> See [kernel/abi/linux/syscall_coverage.md](kernel/abi/linux/syscall_coverage.md) for per-interface compatibility levels.

## Hardware and virtual machine platforms

Individual platform documents in [`docs/zh/platforms/`](docs/zh/platforms/README.md) describe hardware scope, execution modes, dependencies, building and deployment. This README does not maintain a device inventory, prices or a blanket "supported" label. Release commitments are defined in the [platform scope](docs/zh/releases/v1.0.md#2-平台范围).

`tools/a20 boards` queries board sources, `tools/a20 list` lists instances, and `tools/a20 show <实例名>` shows their actions and resource requirements. These queries do not establish physical hardware readiness. NOMMU is an execution mode selected per architecture and instance; memory capacity alone does not establish that a board has that port.

## Current networking limitations

- `NET_STACK=lwip` defaults to the unmodified upstream lwIP 2.2.1 submodule, with a serialized core and `NET_LANES=1`; applications and I/O can still use SMP. `NET_STACK=native` is an A20OS-maintained lwIP derivative extracted from the earlier fork, retaining lane/CUBIC extensions and the original copyright notices.
- Non-embedded 64-bit instances use `NET_STACK=bsd`, running the real protocol stack from a pinned F-Stack version; 32-bit, MCU, NOMMU and embedded boards retain lightweight configurations. See the [completion record](docs/zh/net/router-completion.md) for per-architecture runtime validation and remaining software-router work. There is no separate legacy backend or silent fallback.
- TCP uses the real protocol LISTEN/connection path by default. `a20.tcpmode=fast` is a historical local socket-pairing shortcut for tests, not a protocol performance or external-service acceptance path.
- `NET_ROLE=router` enables IPv4/IPv6 forwarding. A real three-node KVM setup has covered dual NICs, dynamic NAPT source-port conflicts, gateway/source policy routing and Linux dnsmasq DHCP/DNS. Upstream DHCP uses `--dhcp-broadcast`; dynamic SIOCSARP is not yet supported.
- ICMP RELATED translation and forwarding MTU errors are integrated. Full fragmented NAT and local PMTU/error queues remain incomplete, so production software-router maturity cannot yet be claimed. The default conntrack=256 is a measured connection-capacity limit. The upstream server profile passed 320 connections with tracking enabled; fixed INET staging storage still costs about 30 KiB per connection.
- The upstream stack provides Reno; CUBIC and some per-socket TCP buffer adjustments belong to native. Unsupported options return errors. NIC checksum offload is currently disabled; the porting layer computes software checksums for upstream forwarding.

See [network architecture](docs/zh/net/architecture.md), [upstream integration](docs/zh/net/lwip-upstream.md), [BSD integration](docs/zh/net/bsd-port.md) and [comparison methods and data](docs/zh/net/benchmark-method.md) for architecture, build options and remaining work.

## Build and run

### 1. First run: RISC-V 64 serial system

Start with the RISC-V 64 QEMU instance on Linux. It requires no development board, graphical interface or toolchains for other architectures. This is a development version; see [status and gaps](docs/zh/status/README.md) for desktop, server and physical hardware validation scope.

```bash
git clone --recurse-submodules https://github.com/fQwQf/A20OS.git
cd A20OS
# 已经普通 clone 的用户补执行：git submodule update --init --recursive
```

Install the single-architecture dependencies and Rust from the [first-run guide](docs/zh/dev/build.md#首次运行linux--risc-v-64), or prepare its container environment, then run:

```bash
python3 tools/check_quickstart.py             # 缺工具、Rust target、子模块时给出提示
tools/a20 run qemu-riscv64 -- -j4             # 编译完整用户态并启动，保持默认 both ABI
```

At the shell prompt, `/bin/echo hello` should print `hello`; `poweroff` should shut down normally and return to the host terminal. Run `make -j4 smoke-quickstart` for automated acceptance of the shell, external-program output and normal QEMU exit. This covers the complete development image; `smoke-riscv64` only covers kernel bring-up.

Budget 8 GiB of host memory and 20 GiB of free disk space. These are preparation budgets, not minimum hardware specifications. See the [build and run guide](docs/zh/dev/build.md) for complete instructions, resource waits, exit methods and troubleshooting.

### 2. Other build entry points

Bare `make`/`make all` at the repository root is the dual-architecture release build, rather than a single-architecture development build. It builds RISC-V64 and LoongArch64 with `PROFILE=benchmark`, 8 cores and embedded drivers, producing `kernel-rv`, `kernel-la`, `disk.img` and `disk-la.img`.

For everyday development, specify `ARCH`, `BOARD` and `run`:

```bash
# RISC-V 64 开发镜像并在 QEMU 中运行
make ARCH=riscv64 BOARD=qemu-virt-riscv64 run
```

You can also use **instance-based entry points**. TOML files under `instances/` declare complete configurations; `tools/a20` validates and executes them. The following `run-*`/`debug-*`/`smoke-*` make targets are thin wrappers:

```bash
tools/a20 list                     # 列出所有预定义实例
tools/a20 run qemu-riscv64         # 等价于 make ARCH=riscv64 run
tools/a20 run xfce-x86_64          # 图形桌面实例
tools/a20 debug qemu-riscv64       # GDB 调试实例
tools/a20 test smoke-riscv64       # 冒烟测试实例
```

See [docs/zh/dev/instances.md](docs/zh/dev/instances.md) for instance fields, component registries and validation checks.

For other platforms, consult their individual documents for builds, firmware and installation media.

*Advanced compiler options:*

* `OPT="-O3"` / `OPT="-O0 -g -DDEBUG"`: kernel optimization and debug options.
* `NR_CPUS=N`: CPU count. The verified QEMU SMP subset accepts this directly; other boards require `ALLOW_UNVERIFIED_SMP=1` for an explicit bring-up experiment.

### 3. Compilation cache (optional)

The build system transparently supports [ccache](https://ccache.dev). When installed, it accelerates kernel, user-space, native-test and driver-package compilation, reducing elapsed time for repeated builds with the same parameters.

```bash
# 安装 ccache（Debian/Ubuntu）
sudo apt install ccache
```

No configuration is required: detection enables it automatically. Without it, build behavior, functionality and validation checks remain unchanged. To disable it temporarily, supply an empty override:

```bash
make CCACHE= ...
```

ccache only accelerates local host-side iteration. It does not affect build-time measurements performed by benchmark workloads inside the guest.

## Package management and image distribution

A20OS uses apk (the Alpine package format). Declarative recipes under `packages/recipes/` package the kernel, user space, drivers and ports as standard apk packages. These are combined with upstream Alpine packages according to the world manifests in `packages/world/`, with GitHub Actions handling multi-architecture builds, signing and publishing.

```bash
make ARCH=riscv64 image-world PKG_WORLD=base   # 打包 → 建库 → 组镜像，一步到位
```

Full documentation: [docs/zh/packaging/](docs/zh/packaging/overview.md), covering the overview, recipe reference, world assembly, repositories and signing, CI/CD and migration.

## Testing and quality assurance

A20OS maintains validation checks for low-level development. Contributors can run local `Makefile` targets before submitting code:

* **Basic bring-up**: `make smoke-riscv64` only boots a `BRINGUP=1` kernel-only image. The serial log must contain `part ok` and `System is going down for power-off NOW`, demonstrating that the kernel completed bring-up and shut down itself. Timeout exit code 124 is a failure. This does not test system calls.
* **System calls**: `make smoke-abi-linux` runs Linux ABI syscall smoke tests in QEMU; `smoke-syscall-ext` covers extended syscalls such as keyring/AIO/acct/fanotify/pidfd/io_uring/landlock/userfaultfd/perf/fileattr.
* **Load and stress**: concurrency checks including `smoke-sched-stress` and `smoke-vfs-stress` catch subtle deadlocks and crashes.
* **User-space services**: `make smoke-native-fs-all` validates the four ufsd backends managed by svcmgr (FAT/ext4 read-write and ISO9660/NTFS read-only) and recovery from SIGKILL crashes. `smoke-native-svc`/`smoke-native-registry` cover supervisor recovery and rebinding by name.
* **Architectural compliance**: checks such as `make check-concurrency-foundation` inspect compliance with SMP locking contracts at build time.
* **Networking**: `make smoke-network-suite` covers sockets and protocol stacks; `smoke-net-accept` covers inbound accept on a real lwIP LISTEN pcb; `smoke-netfilter-nat` exercises DNAT through QEMU `hostfwd` (host 18081 → guest 18082) and verifies an echo from a host probe; `smoke-ct-capacity` fills conntrack to `ct_capacity`, checks that LRU evicts the least recently used entry and then checks expiry with a short timeout; `test-nat-rewrite` asserts SNAT/MASQUERADE address, port and reply-tuple rewriting on the host; `smoke-net-tcp-lanes` checks TCP structure with multiple lanes.

## Contributing

Contributions from the open-source community are welcome as we explore operating system architecture together.

* Before opening a Pull Request, run the relevant local validation checks to detect new data races and regressions.
* Consult [a20os-improvement-todo.md](docs/zh/roadmap/a20os-improvement-todo.md) for current engineering bottlenecks and possible contribution areas.

## Design documentation

For the operating system design, engineering bottlenecks, approaches and concurrency model, see:

* [Operating system architecture (architecture.md)](docs/zh/architecture.md)

## Repository layout

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

## License and acknowledgments

* The main project code is licensed under **Apache 2.0**. See **[LICENSE](LICENSE)**.
* Centralized license declarations for third-party components are in **[docs/THIRD_PARTY_NOTICES.md](docs/zh/THIRD_PARTY_NOTICES.md)**.
* See **[docs/ACKNOWLEDGMENTS.md](docs/zh/ACKNOWLEDGMENTS.md)** for integrated and referenced projects, public standards and acknowledgments.
* Images such as `disk.img` and `build/images/<world>-<arch>.img` are build artifacts. Distribution obligations depend on their components, linkage and exact versions. Some third-party sources are tracked trees and others are submodules; no single model covers them all. Check each component against the [third-party notices](docs/zh/THIRD_PARTY_NOTICES.md) before distribution.
