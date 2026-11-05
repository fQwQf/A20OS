# Hypervisor 使用指南

本文说明 A20OS Hypervisor 的多架构使用与验证流程。从仓库根目录执行开发机命令；标注“A20 宿主 shell”的命令在启动后的 A20OS 中执行。先用无 rootfs 的内核测试确认后端可用，再按需构建用户态或 RISC-V 完整 guest 环境。RISC-V 架构下统一实现与验收参见 [RISC-V SBI 增补](10-riscv-sbi.md)。

## 1. 支持范围与前提

| A20 宿主架构 / 后端 | 必要条件 | guest 初始模式 / RAM 基址 | 本地验收 |
|---|---|---|---|
| riscv64 / H | H、Sstc，firmware 开放 STCE | VS，`0x80000000` | QEMU，2 vCPU |
| riscv32 / H | H、Sstc，firmware 开放 STCE | RV32 VS，`0x80000000` | QEMU，2 vCPU |
| aarch64 / EL2 | 启动时拥有 EL2，MMU 配置 | AArch64 EL1h，`0x40000000` | QEMU，2 vCPU |
| arm32 / HYP | ARMv7 virtualization，启动时拥有 HYP | ARM SVC，`0x40000000` | QEMU Cortex-A15，2 vCPU |
| x86_64 / AMD SVM | SVM、NPT，firmware 未设置 SVMDIS | flat32 CPL0，`0x100000` | QEMU TCG / AMD KVM，2 vCPU |
| x86_64 / Intel VMX | VMX、EPT、unrestricted guest、INVEPT、preemption timer，firmware 允许 VMX | flat32 CPL0，`0x100000` | Bochs 软件 CPU，2 vCPU |
| loongarch64 / LVZ 实验 | LVZ v1、8 个 scratch CSR、MATP_ROOT、显式实验参数 | 整数 guest，`0x80000000` | 仅编译与无 LVZ 负控 |

同一 ISA 的宿主与 guest 配对；没有跨 ISA guest 执行。ARM 后端需要启动阶段保留的 monitor，不能在已由其它 hypervisor 占用 EL2/HYP 的环境中直接启用。`CONFIG_NOMMU` 不提供这些原生二级页表后端。

RV64 的完整 A20OS RAMFS guest 和管理器有独立运行路径。ARM/x86/RV32 当前验收载荷为小型 guest 指令程序，没有覆盖完整 Linux、Windows 或 A20OS SMP guest 引导。VMX 没有 Intel 实机证据；LoongArch 没有 LVZ 正控证据。当前也没有 A20 `/dev/kvm` 兼容层，不能据此在 A20 内直接运行 QEMU/KVM。详见 [后端说明](08-multiarch-backends.md) 和 [验证范围](09-smp-validation.md)。

### 1.1 开发机依赖

通用构建依赖按 [构建指南](../dev/build.md) 准备，包括 Python 3.11+、GNU make、仓库配置的 Rust 工具链与所选架构 GCC/binutils。编译器常见名称：

| 架构 | 编译器 | 外层模拟器 |
|---|---|---|
| riscv64 | `riscv64-unknown-elf-gcc` 或仓库识别的 RISC-V ELF 工具链 | `qemu-system-riscv64` |
| riscv32 | 带 RV32 multilib 的 RISC-V ELF 工具链，或 RV32 交叉编译器 | `qemu-system-riscv32` |
| aarch64 | `aarch64-linux-gnu-gcc` | `qemu-system-aarch64` |
| arm32 | `arm-linux-gnueabihf-gcc` | `qemu-system-arm` |
| x86_64 | `x86_64-linux-gnu-gcc` | `qemu-system-x86_64`；VMX 用 Bochs |
| loongarch64 | `loongarch64-linux-gnu-gcc` | `qemu-system-loongarch64`，仅无 LVZ 负控 |

内核测试不需要磁盘镜像、rootfs、GUI 或 root 权限。完整宿主镜像还需要`qemu-img`、mtools、dosfstools、e2fsprogs、xorriso 等镜像工具。KVM 验证需要开发机能访问 `/dev/kvm`，并向外层 A20 虚拟机暴露嵌套 SVM/NPT；当前 KVM 验证环境基于 AMD 平台。RISC-V 测试使用 QEMU 自带的默认 OpenSBI；已验证版本为 QEMU 10.0.13 / RV32 OpenSBI 1.6，旧 firmware 若不开放 STCE 会被拒绝。

## 2. 最快运行：不依赖 rootfs 的内核测试

以下每条命令从当前源码构建新内核，再启动、检查结果并等待模拟宿主关机。默认 `KERNEL_WERROR=1`。`--cpus 2` 同时设置构建的 `NR_CPUS=2` 和模拟 CPU 数量，并开启双 guest vCPU 测试；省略时为单 CPU。

```sh
python3 tools/qemu/check_hyp_multiarch.py --arch aarch64 --cpus 2 --build
python3 tools/qemu/check_hyp_multiarch.py --arch arm32 --cpus 2 --build
python3 tools/qemu/check_hyp_multiarch.py --arch riscv32 --cpus 2 --build
python3 tools/qemu/check_hyp_multiarch.py --arch riscv64 --cpus 2 --build
python3 tools/qemu/check_hyp_multiarch.py --arch x86_64 --cpus 2 --build --accel tcg
# 下面一项只用于可暴露嵌套 SVM 的 AMD KVM 开发机：
python3 tools/qemu/check_hyp_multiarch.py --arch x86_64 --cpus 2 --build --accel kvm
```

Intel VMX 的实际执行命令见第 3 节。当前 QEMU runner 的 x86 正控按 SVM 检查后端名称；不能将其 KVM 选项当作 Intel VMX 的现成验收入口。

负控对上述每个架构加 `--without-virtualization`：

```sh
python3 tools/qemu/check_hyp_multiarch.py --arch arm32 --cpus 2 --build --without-virtualization
python3 tools/qemu/check_hyp_multiarch.py --arch riscv32 --cpus 2 --build --without-virtualization
# LoongArch 本地仅支持此项，runner 会在 UART 提示后发送显式 LVZ 实验参数：
python3 tools/qemu/check_hyp_multiarch.py --arch loongarch64 --cpus 2 --build --without-virtualization
```

ARM 负控关闭 virtualization，RISC-V 设置 `h=false`，x86 关闭 SVM/VMX。 LoongArch TCG 本身没有 LVZ；该选项检查显式 opt-in 也不能绕过硬件能力检查，不表示执行了 LVZ。成功负控应打印 `HYP_ARCH_TEST: SKIP (backend unavailable)`并正常关机。

### 2.1 参数、产物与启动参数

| runner 参数 | 用途 |
|---|---|
| `--arch ARCH` | 选择上表中的架构 |
| `--build` | 用 `BRINGUP=1 NR_CPUS=N kernel-only` 构建；与 `--kernel` 二选一 |
| `--kernel PATH` | 运行已有镜像；调用者保证它与当前源码及 CPU 数量一致 |
| `--cpus 1` / `--cpus 2` | 单核生命周期 / 双核并行会合测试，默认 1 |
| `--jobs N` | 构建并行数，默认 8 |
| `--accel tcg` / `--accel kvm` | 默认 TCG；KVM 仅开放给 x86 runner |
| `--without-virtualization` | 要求 SKIP 和关机，出现正常 guest PASS 会判失败 |
| `--timeout SEC` | 模拟器运行超时，默认 90 秒，不包括编译 |
| `--log PATH` | 指定运行日志位置；默认在 `.kernel-build/smoke/` |

构建日志为 `.kernel-build/smoke/hyp-ARCH-build.log`。运行日志例如`hyp-arm32-smp-tcg.log`、`hyp-x86_64-smp-kvm.log`，负控增加 `-disabled`。构建输出中的 `Kernel-only build complete:` 给出实际镜像路径，不要假定所有构建配置共享同一个内核文件。

ARM32/AArch64 必须给 runner 传 `kernel.bin`，使 QEMU 按 raw Linux handoff 提供 DTB 与 bootargs；RV32/RV64/x86 使用 `kernel.elf`。LoongArch 构建额外设置 `UART_CMDLINE=y`，runner 等待 UART command prompt 后发送参数。

手动 QEMU 引导时需设置 `a20.hyp_test=1`；双核再加 `a20.hyp_smp=1`，并保证构建与 `-smp` 都为 2。例如：

```sh
make ARCH=arm32 BRINGUP=1 NR_CPUS=2 kernel-only -j8
# 将 PATH 换成上一步打印的实际路径：
qemu-system-arm -machine virt,virtualization=on -cpu cortex-a15 -accel tcg \
  -m 1G -nographic -smp 2 -kernel PATH/kernel.bin \
  -append 'a20.hyp_test=1 a20.hyp_smp=1'
```

AArch64 改用 `qemu-system-aarch64` 与 `cortex-a57`。RISC-V 使用`-machine virt -cpu rv32,h=true` / `rv64,h=true` 和 `-bios default`。 x86 的 QEMU 引导还需通过 `-fw_cfg name=opt/x86/cmdline,string=...` 传入相同参数，优先使用 runner 避免漏项。测试选项是精确匹配的独立 boot token，普通启动不设置这些参数就不会自动执行该测试。

### 2.2 如何判断通过

以 ARM32 双核正控为例，关键输出应同时存在：

```text
HYP_SELFTEST: PASS (vmid=0 entries=9)
HYP_ARCH_TEST: NX+RO PASS
HYP_ARCH_TEST: preemption+stop PASS
HYP_ARCH_TEST: PASS backend=arm32-hyp rounds=3
HYP_SMP_TEST: ready peak=2
HYP_SMP_TEST: PASS backend=arm32-hyp cpus=0,1 peak=2 step=stopped
System is going down for power-off NOW
```

`entries` 因页表格式而异，不应硬编码为 9。三轮 guest 输出 `HYP` 并请求关机，同时检查第二页按需补页和内容读回。NX/RO 让真实 guest 取指/写入失败，要求 FAULT 且只读数据不变。除 RV64 尚无对应 spin payload 外，正控还要求屏蔽中断的无限循环被宿主抢占，并能 STOP。

SMP 不是串行运行两次：两个固定到不同宿主 CPU 的 worker 各调用一个 vCPU 的 RUN，guest 各写 ready 后等待宿主 release；控制器必须观察到两个 ready 和并发 RUN 峰值 `peak=2`，才释放两个 guest 并等待 CPU_OFF。最后要求 VM/vCPU/映射/页表计数回到基线。测试由宿主调用 hart_start 启动次 vCPU； guest CPU_ON/HSM 包装已实现，作为后续演进路径。

runner 检查所需 PASS、宿主关机、进程返回值和 FAIL/PANIC 标记，返回 0 才算通过。只看到启动 banner、`HYP` 或 SKIP 都不能代替正控。

## 3. Intel VMX：使用 Bochs 实际执行

本地 AMD 开发机不能用 KVM 执行 Intel VMX，QEMU TCG 也不实现 VMX。可从上游 Bochs 3.0 构建带 VMX、SMP 与 debugger 的软件 CPU。准备 C/C++ 编译器、 make、curl 与 tar，在自行选择的临时构建目录执行：

```sh
curl -L https://github.com/bochs-emu/Bochs/archive/refs/tags/REL_3_0_FINAL.tar.gz -o bochs.tar.gz
tar xf bochs.tar.gz
cd Bochs-REL_3_0_FINAL/bochs
./configure --enable-vmx=2 --enable-smp --enable-x86-64 --with-nogui \
  --disable-docbook --enable-debugger --enable-disasm
make -j8
```

回到 A20OS 仓库根目录构建并执行；替换两个 PATH：

```sh
make ARCH=x86_64 BRINGUP=1 NR_CPUS=2 kernel-only -j8
python3 kernel/arch/x86_64/hyp/check_vmx_bochs.py \
  --bochs-dir PATH/Bochs-REL_3_0_FINAL/bochs --kernel PATH/kernel.bin \
  --cpus 2 --output .kernel-build/vmx-bochs
python3 kernel/arch/x86_64/hyp/check_vmx_bochs.py \
  --bochs-dir PATH/Bochs-REL_3_0_FINAL/bochs --kernel PATH/kernel.bin \
  --cpus 2 --without-virtualization --output .kernel-build/vmx-bochs-disabled
```

这里必须使用 `kernel.bin`，由 BIOS、最小 Multiboot loader 和 debugger 装载；无需 GRUB/rootfs。runner 分 4 KiB 装载以避开上游 Bochs 3.0 loadmem 多块地址递增问题，没有修改模拟器 CPU 实现。`--timeout` 默认 120 秒。成功后端名为 `x86-vmx`，仍要求三轮生命周期、NX/RO、抢占/STOP 和 SMP。日志 `serial.log`、`bochs.log`、`console.log` 及配置保存在 `--output` 下。 Bochs 的 ACPI 关机可能返回 1，runner 根据完整标记和关机记录裁决。这是 VMX/EPT 指令的软件 CPU 执行证据，没有测得 Intel 实机性能或稳定性。

## 4. A20 宿主 shell：运行用户态最小 guest

内核测试无需用户态。要运行 syscall 路径，还需构建包含 `/bin/hyp_test` 的 Linux/both ABI 完整 A20 宿主镜像并启动到 shell，参照通用构建指南。单独编译这个命令可用：

```sh
make -C user ARCH=arm32 build/arm32/hyp_test -j8
make -C user ARCH=riscv32 build/riscv32/hyp_test -j8
make -C user ARCH=aarch64 build/aarch64/hyp_test -j8
make -C user ARCH=x86_64 build/x86_64/hyp_test -j8
```

A20 宿主 shell 执行 `hyp_test`。它创建 VM、装入相同 ISA 的小型载荷、运行至 shutdown，检查输出计数并销毁。通过标记为 `HYP_VCPU_TEST: PASS`。 x86 根据 SVM CPUID 选择 VMMCALL 或 VMCALL；ARM 使用 HVC。`hyp_test echo`仅有 RISC-V UART 载荷，消耗宿主输入中的三个字节，避免用在共享交互输入上。四架构命令均已编译链接；多架构 syscall 端到端全量运行仍在持续收敛。

用户态公共包装在 `user/cmds/core/hyp/hyp_guest.h`，Native 和 Linux 扩展 ABI 最终调用同一内核 VM/vCPU 核心。Linux 扩展编号在`kernel/include/core/syscall_nr.h`；Native 使用自己的 handle/权限表，不能把 Linux 编号直接用于 Native 调用。

### 4.1 编写 VMM 与 SMP 调用顺序

1. 探测后端，创建 VM；内存大小非零且按 4 KiB 对齐，受后端 GPA 范围限制。默认 RAM 基址见第 1 节，ELF/FDT 装载器不能继续硬编码 RISC-V 地址。
2. 多 vCPU 在创建 vCPU 前调用 `hyp_guest_vm_smp(vm, count)`；内核对应`hyp_vm_configure_smp`。同时启用 managed console，分别处理每 VM 的 console_read/write，避免 legacy console 的单 runner 约束。
3. 通过 vm_load 装入 guest 程序/引导数据，再按 id 创建 vCPU 和设置启动寄存器。同一 vCPU 的 RUN 不可并发；每个运行线程持有自己的 vCPU 引用。
4. 一个宿主线程运行一个 vCPU，根据返回原因处理 I/O、让出调度或等待重启。次 vCPU 初始停止，经对应 ISA 的 HSM/PSCI/显式启动 ABI 激活。
5. 销毁前 STOP 所有 runner，等待线程退出，释放全部 vCPU 和 VM 引用。 STOP 与 guest CPU_OFF 不同：后者是可再次启动的 hart 停止状态。

公共 VM 上限 `HYP_MAX_VCPUS=8`，实际并发数还受后端和宿主 CPU 配置限制；当前实测验证并发数为 2。增加 `NR_CPUS` 或传更多 vCPU 参数不构成有效验证。

| RUN 返回原因 | VMM 处理 |
|---|---|
| `YIELD` | 可恢复；处理 I/O/调度后再次 RUN |
| `PAUSED` | 等待控制器恢复 |
| `HART_STOPPED` | 等待 CPU_ON/HSM 启动，不能误作整台 VM 关机 |
| `SHUTDOWN` | 停止其它 runner 并回收该 VM |
| `STOPPED` | 控制器已取消运行，结束 runner |
| `FAULT` / `ERROR` | 保存 PC、cause、stval、htval 等诊断后停止并回收 |

C API 返回错误与 guest FAULT 是两层结果。`EBUSY` 表示更新与运行或同 vCPU RUN 冲突，不应忙循环；让出调度并协调持有者。native stage-2 只允许并行新增映射，替换已有叶子返回 EEXIST；远端 vCPU 活跃时 unmap 返回 EBUSY。先停止相关 vCPU，再修改映射并回收 backing。内核调用者的 guest 执行权限用`HYP_PROT_EXEC`，ARM32 不可用宿主 `PTE_X=0` 代替。

各 ISA 的 reset 寄存器、hypercall、支持的 CSR/MSR 和设备范围分别见[ARM32](../../kernel/arch/arm32/hyp/README.md)、[AArch64](../../kernel/arch/aarch64/hyp/README.md)、[RV32](../../kernel/arch/riscv32/hyp/README.md)、[x86](../../kernel/arch/x86_64/hyp/README.md)。

## 5. 完整 RISC-V A20OS guest 与多 VM 管理

`hypvm`、`hyp_boot`、guest ELF/FDT 装载器和 `a20-vmd` 的完整 OS 工作流仍面向 RV64。它们没有随硬件后端自动获得 ARM/x86 完整 OS 引导能力。

```sh
tools/a20 build hyp-host-riscv64 -- -j4
tools/a20 run hyp-host-riscv64 --name lab --detach --no-build
tools/a20 attach lab
```

该模板使用 ABI=both、1 CPU、1 GiB RAM、TCG，显式启用 H/Sstc，打包 RAMFS guest 和用户程序。命名运行有独立磁盘 overlay、宿主 UART 与管理 socket，应看到 `A20VMD: READY protocol=2`。`Ctrl-]` 断开控制台后宿主继续运行。

```sh
tools/a20 vm start a20-ramfs-riscv64 --host lab --name guest-a
tools/a20 vm start a20-ramfs-riscv64 --host lab --name guest-b
tools/a20 vm list --host lab
tools/a20 vm console guest-a --host lab
```

另一个终端连接 guest-b。等待实际 shell 提示符后执行命令；START accepted 只代表接受请求，不能代替 console/list 中的启动结果。默认 guest 为 128 MiB、 1 vCPU、仅 RAMFS，无磁盘和网络。控制与清理命令：

```sh
tools/a20 vm pause guest-a --host lab
tools/a20 vm resume guest-a --host lab
tools/a20 vm stats --host lab
# 正常结束：在 guest shell 输入 exit，等 list 显示 exited 后：
tools/a20 vm destroy guest-a --host lab
# 对另一台尚在运行的 guest，可先强制停止再等退出：
tools/a20 vm stop guest-b --host lab
tools/a20 vm list --host lab
tools/a20 vm destroy guest-b --host lab
tools/a20 stop lab
tools/a20 destroy lab
```

资源不足时 runner 会等待，可通过 `A20_WAIT_TIMEOUT=60` 和`A20_PREFLIGHT_TIMEOUT=60` 限定等待时间。外层 stop 退出整个 QEMU；destroy 删除停止实例的临时磁盘和运行目录，先保存需要的 guest 数据。完整管理约束、传输协议和控制台重连见 [宿主运行目录](06-host-runtime.md) 与[guest supervisor](07-guest-supervisor.md)。

在 A20 shell 也可运行：

```sh
hypvm --managed -k /bin/boot/guest-kernel-ramfs.elf -m 128
hypvm --help
```

并行进程需独立 stdin/stdout 管道或 PTY。仓库保留`hyp-host-riscv64-smp2` 与 `a20-ramfs-riscv64-smp2` 实验 profile，必须构建匹配的 SMP 宿主与 guest 内核。小型载荷 SMP 通过不代表完整 RV64 OS guest 的 trap frame 边界全部收敛，不能据此将这些 profile 的完整 OS SMP 标为已验收。

双 guest 真实场景检查：

```sh
python3 tools/probes/hyp_pair_probe.py --host lab --timeout 120 --report /tmp/a20-pair/report.json
```

它检查 shell 命令、控制台隔离、重连、暂停隔离、正常退出、记录销毁和计数基线；证据属于独立完整 guest 场景，单次通过不代表重复运行或长稳稳定。

## 6. LoongArch 实验开关

LoongArch 后端默认不可用，未来 LVZ 硬件实验需显式传`a20.lvz_experimental=1`，且所有硬件能力检查通过。开关不会模拟缺失的 LVZ。本地只执行第 2 节负控，当前 runner 会拒绝请求 LoongArch 正控。

实现含整数/GCSR bank、原生 GPA root、entry/exit、HVCL 小载荷和公共 SMP 接线；guest FP/vector/LBT、guest timer、完整设备/OS 启动未实现，真实 LVZ 下的隔离、抢占和 SMP 均未验收。部署实验前阅读[LoongArch 后端](../../kernel/arch/loongarch64/hyp/README.md)。

## 7. 常见问题与复测

| 现象 | 检查方法 |
|---|---|
| 编译器不存在 / RV32 ABI 不匹配 | 检查第 1 节工具链、RV32 multilib 和构建指南；保留 build log |
| 没有 HYP 测试行 | 是否传精确 boot token；ARM 是否 raw bin；Loong 是否 UART 命令行构建 |
| 正控打印 SKIP / 创建返回 errno=95 | 查 H/Sstc/STCE、EL2/HYP handoff、SVM/NPT 或 VMX 所需能力；关闭扩展的负控应 SKIP |
| ARM 单核能启而双核卡住 | 构建 `NR_CPUS`、QEMU `-smp` 与 runner `--cpus` 是否一致；宿主是否观察到两个 online CPU |
| SMP ready 超时 / peak=1 | 检查 worker affinity、managed console、次 vCPU 状态及最后的 PC/cause；保留完整日志 |
| KVM 不可用 | 检查 `/dev/kvm` 访问和嵌套 SVM；可用 TCG 验 SVM、Bochs 验 VMX |
| guest 立即 FAULT | 检查 ISA、初始模式、entry/RAM 基址、执行权限、hypercall 与支持的设备；不要按完整 PC/virt 机型假定设备齐全 |
| manager 找不到 | 查看宿主日志是否 READY，模板是否 `vm_manager=true`、是否命名运行；guest console 与宿主 attach 不同 |
| 管理请求超时 | 保存宿主/guest 日志；stop guest 无法恢复时退出外层宿主再重建 |

无需模拟器的相关检查：

```sh
python3 -m unittest tools.tests.test_hyp_s2 tools.tests.test_hyp_backend_contract \
  tools.tests.test_arch_boundary tools.tests.test_a20_trim -v
make check-trim-registry
```

实际测试结果、最终镜像/日志 SHA-256 和尚未覆盖的物理板、虚拟中断控制器、完整 OS SMP、长稳范围见 [验证记录](09-smp-validation.md)。构建缓存与运行日志在 `.kernel-build/`，确认没有正在使用的进程并保存所需证据后，可删除自行创建的测试输出目录；无需删除其它实例、worktree 或开发任务的缓存。
