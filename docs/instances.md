# 实例化构建与运行（instances/ 与 tools/a20）

A20OS 的构建、运行与冒烟测试配置统一由 **实例清单** 声明：`instances/` 目录下的每个 TOML 文件描述一个完整的、可构建、可运行、可测试的系统实例（架构、板卡、ABI、内核选项、机器配置、根文件系统、网络、GUI、测试期望）。`tools/a20` 读取实例、做完整校验、推导出对应的 Makefile 变量，再调用 make 执行——**Makefile 仍是唯一的构建引擎**，实例层只负责"配置声明 + 变量推导"，两者通过 `make -n` 输出的 QEMU 命令行保持一致。

设计要点：

- **未写的字段不落任何变量**，直接落回 Makefile 默认值。策略只有一个出处（Makefile），实例只携带自己的增量。
- **校验前置**：架构/板卡/ABI/SMP/NOMMU/驱动组件等约束在启动编译前全部检查完毕，错误信息指向具体字段。
- **声明即门禁**：`make check-instances`、`check-instance-matrix`、`check-component-registry` 在 CI 中保证实例、架构矩阵、驱动注册表与构建系统永不漂移。

## 快速上手

```bash
tools/a20 list                            # 列出所有实例（名称、架构、形态、状态）
tools/a20 run qemu-riscv64                # 构建并在 QEMU 启动（文本模式）
tools/a20 run qemu-x86_64-gui             # GUI 实例（virtio-gpu + 声卡）
tools/a20 debug qemu-riscv64              # -O0 -g 构建 + QEMU GDB stub（:1234）
tools/a20 test smoke-riscv64              # 跑一个冒烟测试实例
tools/a20 flash stm32f103-xuanwu          # 构建固件并经 OpenOCD 烧录开发板
tools/a20 package vbox-iso-x86_64         # 构建并组装发布产物（ISO/UEFI 镜像/SD 卡/发布件）
tools/a20 build qemu-riscv64 -- -j8       # 只构建；`--` 后参数透传给 make
tools/a20 show-vars qemu-riscv64-smp4     # 查看实例推导出的 make 变量
tools/a20 check                           # 校验全部实例（CI 门禁同款）
tools/a20 check-registry                  # 校验驱动组件注册表并与 Makefile 交叉比对
```

实例参数既可以是 `instances/` 下的名字（`qemu-riscv64`），也可以是任意 TOML 文件路径。`--dry-run` 打印将执行的 make/QEMU 命令而不执行。

需要 Python ≥ 3.11（只用标准库，无第三方依赖）。

## 实例文件参考

最小实例只有两个字段：

```toml
name = "qemu-riscv64"        # 可选；默认取文件名主干
arch = "riscv64"             # 必填
```

完整示例：

```toml
name = "qemu-riscv64-gui"
description = "RISC-V 64 GUI desktop in QEMU with virtio-gpu"
arch = "riscv64"
board = "qemu-virt-riscv64"  # 可选；默认 qemu-virt-<arch>（armv7m 默认 stm32f103）
abi = "both"                 # linux | native | both

[kernel]
profile = "full"             # full | benchmark | mcu
opt = "-O3"
user_opt = "-O3"
bringup = false              # true = 只编译内核，不组文件系统镜像
nommu = false                # 仅 riscv64/riscv32/aarch64/arm32/armv7m
driver_deployment = "generic"  # generic（.a20drv 可加载）| embedded（静态链接）
ubsan = true
swap = false
werror = true
cooperative_boot = false
storage_read_only = false
external_root = false
ramfs_user = false           # 仅 loongarch64

[machine]
smp = 1                      # >1 仅限已验证 QEMU 平台，否则需 allow_unverified_smp
memory = "1G"                # QEMU -m 参数格式
allow_unverified_smp = false
extra_qemu = ["-device", "riscv-iommu-pci,bus=pcie.0"]  # 追加的 QEMU 参数（测试用）

[gui]
enabled = false              # true = virtio-gpu GUI 启动
display = "gtk"              # QEMU -display
audio_driver = "pa"          # QEMU audiodev 后端
audio_device = "hda"         # hda | virtio
frame_window = 15            # GUI 冒烟首帧窗口（秒）

[net]
hostfwd = ["tcp::5555-:5555", "udp::5555-:5555"]  # 空列表 = 不做端口转发

[rootfs]
size_mb = 128                # FAT32 根盘大小
gui_size_mb = 512            # GUI 根盘大小
ext4_size_mb = 128
extra_size_mb = 2048
world = "base"               # packages/world/<name>.world 必须存在
extra_packages = ["vim", "git", "gcc"]
drivers = ["virtio-net", "hda"]   # 运行时驱动子集，见下文"组件注册表"

[test]                       # 冒烟测试实例（a20 test）
timeout = "20s"              # 默认 20s
input_delay = 8              # 注入命令前等待秒数，默认 8
commands = ["syscall_smoke", "poweroff"]  # 启动后经串口注入的命令
expect = ["SYSCALL_SMOKE: PASS"]          # 日志中必须全部出现的子串

[stm32]                      # 仅 arch = "armv7m"：STM32 板级变体
flash_kb = 512               # → STM32_FLASH_KB
ram_kb = 64                  # → STM32_RAM_KB
xuanwu = true                # 普中玄武板（STM32F103ZET6）
qemu = true                  # 面向 QEMU stm32vldiscovery 的构建（a20 run 可用）
bt_name = "KasaneTeto"       # 蓝牙参数（格式约束由 Makefile 校验）
bt_pin = "2233"
bt_baud = 38400
wifi_ssid = "..."            # 可选；留空表示不内置 Wi-Fi 配置
wifi_password = "..."

[flash]                      # a20 flash：烧录开发板（当前支持 openocd/STM32）
tool = "openocd"
interface = "interface/cmsis-dap.cfg"
transport = "swd"
adapter_khz = 1000
serial = ""                  # 可选：CMSIS-DAP 探针序列号（多探针时区分）

[package]                    # a20 package：构建后组装发布产物
kind = "release"             # grub-iso | uefi-image | fit-sdcard | release
variant = "..."              # uefi-image: default|text|gui；fit-sdcard: minimal|sdcard|extra
kernel_out = "kernel-rv"     # 仅 release：产物文件名（缺省按架构惯例）
disk_out = "disk.img"        # 仅 release
```

### 字段到 make 变量的映射

| 实例字段 | make 变量 |
|---|---|
| `arch` / `board` / `abi` | `ARCH` / `BOARD` / `ABI` |
| `kernel.profile` / `opt` / `user_opt` | `PROFILE` / `OPT` / `USER_OPT` |
| `kernel.bringup` / `nommu` | `BRINGUP` / `NOMMU` |
| `kernel.driver_deployment` | `DRIVER_DEPLOYMENT` |
| `kernel.ubsan` / `werror` | `CONFIG_UBSAN` / `KERNEL_WERROR` |
| `kernel.swap` | `CONFIG_SWAP`（y/n） |
| `kernel.cooperative_boot` / `storage_read_only` / `external_root` / `ramfs_user` | `COOPERATIVE_BOOT` / `STORAGE_READ_ONLY` / `EXTERNAL_ROOT` / `RAMFS_USER` |
| `machine.smp` / `memory` / `allow_unverified_smp` | `NR_CPUS` / `QEMU_MEMORY` / `ALLOW_UNVERIFIED_SMP` |
| `gui.display` / `audio_driver` / `audio_device` / `frame_window` | `QEMU_GUI_DISPLAY` / `QEMU_GUI_AUDIO_DRIVER` / `QEMU_GUI_AUDIO_DEVICE` / `GUI_FRAME_WINDOW` |
| `net.hostfwd` | `NET_HOSTFWD`（逗号连接） |
| `rootfs.size_mb` / `gui_size_mb` / `ext4_size_mb` / `extra_size_mb` | `FAT32_IMAGE_MB` / `GUI_FAT32_IMAGE_MB` / `EXT4_IMAGE_MB` / `EXTRA_IMAGE_MB` |
| `rootfs.world` / `extra_packages` | `PKG_WORLD` / `EXTRA_PACKAGES` |
| `rootfs.drivers` | `DRIVER_SELECTION`（自动补 `.a20drv` 后缀） |
| `test.timeout` / `input_delay` | `SMOKE_TIMEOUT` / `SMOKE_INPUT_DELAY` |
| `stm32.*` | `STM32_FLASH_KB` / `STM32_RAM_KB` / `STM32_XUANWU` / `STM32_QEMU` / `STM32_BT_*` / `STM32_WIFI_*` |
| `flash.interface` / `transport` / `adapter_khz` / `serial` | `STM32_OPENOCD_INTERFACE` / `STM32_OPENOCD_TRANSPORT` / `STM32_OPENOCD_ADAPTER_KHZ` / `STM32_CMSIS_DAP_SERIAL` |

`gui.enabled`、`machine.extra_qemu`、`test.commands`、`test.expect`、`flash.tool`、`package.*` 由 a20 自己消费，不产生 make 变量。

### 各动作的适用条件

| 动作 | 说明 |
|---|---|
| `build` | 所有架构；bringup 或 armv7m 实例自动选 `kernel-only`，其余 `dev-build`（文本模式附带 `USER_BUILD_DESKTOP=0`） |
| `run` | 有通用 QEMU 路径的架构；armv7m 需 `[stm32] qemu = true`（走 stm32vldiscovery） |
| `debug` | 通用 QEMU 架构；`-O0 -g` + GDB stub |
| `test` | 通用 QEMU 架构 + `[test].expect` 必填 |
| `flash` | 需要 `[flash]` 段；当前为 armv7m/STM32 OpenOCD 流程（先构建再烧录） |
| `package` | 需要 `[package].kind`：`grub-iso`（x86_64）、`uefi-image`（board=virtualbox-aarch64，variant default/text/gui）、`fit-sdcard`（board=visionfive2，variant minimal/sdcard/extra）、`release`（riscv64/loongarch64） |

VisionFive 2 的 SD 卡编排（firmware 预检、extra 分区来源）保留在 `tools/targets-build.mk` 的 `vf2-*` 目标里——实例提供经过校验的板卡身份与统一入口，编排逻辑不复制进 Python。使用前先按 [platforms/visionfive2-boot.md](platforms/visionfive2-boot.md) 跑一次 `make vf2-firmware`。

### 校验规则（选摘）

- `smp > 1` 只允许在已验证的 QEMU virt 平台（riscv64/aarch64/loongarch64/x86_64），否则必须显式 `allow_unverified_smp = true`。
- `gui.enabled` 与 `kernel.bringup` 互斥；`[test]` 与 GUI 互斥（GUI 冒烟走 `tools/smoke_qemu_gui.py`）。
- `nommu`、`ramfs_user`、`driver_deployment` 都有架构白名单，写错会在编译前被拒绝。
- `run`/`debug`/`test` 仅支持有通用 QEMU 路径的架构；armv7m 走 `tools/stm32.mk`，loongarch32 走 cemu 模拟器。

## 冒烟测试实例

带 `[test]` 段的实例就是一个冒烟测试。`a20 test` 的流程：构建（bringup → `kernel-only`，否则 `dev-build`）→ 从 `make -n _run_impl` 提取该实例的精确 QEMU 命令行（单一事实来源，不复制 Makefile 逻辑）→ 追加 `machine.extra_qemu` → 启动，等待 `input_delay` 秒后经串口注入 `commands` → 在 `timeout` 内等待退出或超时杀掉。

判定规则与旧的手写冒烟一致：**日志中 `expect` 的全部子串都出现即 PASS**（超时杀掉但日志已齐也算 PASS）；否则 FAIL 并打印日志末尾 80 行。日志保存在 `.kernel-build/smoke/<实例名>.log`。

把旧冒烟迁移成实例只需三步：

1. 把 `$(MAKE) ARCH=... ABI=... BRINGUP=...` 一行翻译成实例的顶层/`[kernel]` 字段；
2. 把 `printf` 注入的命令写进 `test.commands`，把 `grep -q` 的模式写进 `test.expect`（注意：expect 是**子串**匹配，不是正则），超时差异写进 `test.timeout`；
3. 额外的 `-device` 参数写进 `machine.extra_qemu`（QEMU 参数顺序无关）；然后把 make 目标改成一行包装：`smoke-foo: ; tools/a20 test smoke-foo`。

已迁移的参考样例：`instances/smoke-riscv64.toml`（纯 expect）、`instances/smoke-abi-linux.toml`（commands + expect）、`instances/smoke-iommu-discovery.toml`（extra_qemu）。

## 组件注册表（components/drivers.toml）

可加载驱动（`.a20drv` 包）的单一事实来源。每个条目：

```toml
[[driver]]
name = "virtio-net"          # 包名 = <name>.a20drv
source = "kernel/drvmod/examples/virtio_net.c"  # 必须存在的源文件
arches = ["riscv64", "x86_64", "aarch64", "loongarch64"]
early_arches = ["riscv64"]   # 可选：嵌入内核 root ramfs 的架构（启动盘驱动）
description = "virtio network device"
```

实例用 `[rootfs].drivers` 按名字选择进入 `/lib/drivers` 的运行时驱动子集；a20 在编译前校验：名字必须存在、必须支持实例架构、不能是该架构的 early 驱动（early 驱动始终嵌入内核镜像，无需也不能再选）。选定后通过 `DRIVER_SELECTION` 传给 make；不写该字段则按 `tools/driver-modules.mk` 的默认全集打包。

`make check-component-registry`（= `tools/a20 check-registry`）会做两层校验：注册表自身（重名、未知架构、early ⊆ arches、源文件存在），以及与 Makefile 的 `DRVMOD_MODULES`/`EARLY_DRVMOD_MODULES` 按架构逐一比对——两边任何一边漂移都会 FAIL。

## CI 门禁

| 目标 | 作用 |
|---|---|
| `make check-instances` | 校验 `instances/` 全部实例（schema + 语义 + 驱动选择） |
| `make check-instance-matrix` | 校验每个 `SUPPORTED_HOSTED_ARCHES` 成员至少有一个有效实例，矩阵与实例目录不漂移 |
| `make check-component-registry` | 校验驱动注册表并与 Makefile 构建清单交叉比对 |

## 与旧 make 目标的对照

旧的逐架构目标全部保留为薄包装，行为不变：

| 旧命令 | 等价的新命令 |
|---|---|
| `make run-riscv64` | `tools/a20 run qemu-riscv64` |
| `make run-riscv64 BRINGUP=1` | `tools/a20 run qemu-riscv64-bringup` |
| `make run-gui-x86_64` | `tools/a20 run qemu-x86_64-gui` |
| `make run-nommu-riscv64` | `tools/a20 run qemu-riscv64-nommu` |
| `make debug-arm64` | `tools/a20 debug qemu-aarch64` |
| `make smoke-riscv64` | `tools/a20 test smoke-riscv64` |
| `make stm32f103-bringup` / `stm32f103-xuanwu` | `tools/a20 build stm32f103` / `stm32f103-xuanwu` |
| `make run-stm32f103-qemu` | `tools/a20 run stm32f103-qemu` |
| `make flash-stm32f103-xuanwu` | `tools/a20 flash stm32f103-xuanwu` |
| `make vbox-iso-x86_64` | `tools/a20 package vbox-iso-x86_64` |
| `make vbox-image-aarch64` / `vbox-text-image-aarch64` / `vbox-gui-image-aarch64` | `tools/a20 package vbox-aarch64` / `vbox-aarch64-text` / `vbox-aarch64-gui` |
| `make vf2-minimal` / `vf2-sdcard` / `vf2-extra` | `tools/a20 package vf2-minimal` / `vf2-sdcard` / `vf2-extra` |
| `make release-rv` / `release-la` | `tools/a20 package release-riscv64` / `release-loongarch64` |

注意：薄包装**不转发**命令行变量覆盖（如 `make run-riscv64 QEMU_MEMORY=2G`）。需要临时覆盖时，要么改实例文件，要么用通用的 `make ARCH=... run`（该变量驱动入口仍然保留）。

## 新增一个实例

1. 在 `instances/` 新建 `<名字>.toml`，必填只有 `arch`；
2. 跑 `tools/a20 check <名字>` 通过校验；
3. 用 `tools/a20 show-vars <名字>` 确认推导出的变量符合预期；
4. `tools/a20 run <名字>` 实际启动验证。

如果新实例覆盖了一个此前没有的架构，记得 `make check-instance-matrix` 会随之更新覆盖关系；新增驱动组件时同步更新 `components/drivers.toml` 并跑 `make check-component-registry`。
