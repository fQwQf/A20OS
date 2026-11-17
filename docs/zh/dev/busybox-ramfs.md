# BusyBox 精简 RAMFS

普通 MMU 精简环境使用一个上游静态 BusyBox 提供 PID 1、ash、文件操作、
诊断及关机命令。内核继续把 ELF 和少量配置嵌入 RAMFS，不需要 world、
磁盘镜像、运行时 apk，也不需要源码构建 musl、mksh、sbase、TLSe 或 fastfetch。

## 构建与运行

```bash
make ARCH=riscv64 RAMFS_USER=1 -j4 kernel-only
# 同一配置直接构建并启动串口 QEMU：
make ARCH=riscv64 RAMFS_USER=1 run
```

`RAMFS_USERLAND=auto` 是默认值。x86_64、aarch64、riscv64、loongarch64 的普通
MMU RAMFS 自动选择 BusyBox。未启用额外程序时，`kernel-only` 和此模式的
`dev-build` 都不会构建源码用户态或附加磁盘；QEMU 不再默认挂载 FAT32 用户态盘。
ARM64 使用 `kernel.bin` 启动，其他上述架构使用 `kernel.elf`。

首次构建从 Alpine CDN 下载固定版本 `busybox-static`，按仓库内
`tools/packaging/busybox.lock.json` 的 SHA-256 验证，只读取包内的 BusyBox ELF
和 `.PKGINFO`。构建工具不执行目标架构二进制。后续构建使用
`build/cache/busybox/<arch>/` 的缓存，重新校验但不联网；内容不变不会重写
ELF，也不会因为 BusyBox 缓存校验而重新链接内核。

可以选择镜像源，或使用自行准备的静态 BusyBox：

```bash
make ARCH=riscv64 RAMFS_USER=1 BUSYBOX_MIRROR=https://mirrors.ustc.edu.cn/alpine kernel-only
make ARCH=riscv64 RAMFS_USER=1 BUSYBOX_BINARY=/absolute/path/to/busybox kernel-only
```

自备程序也会检查 ELF 架构与静态链接属性；必须包含 `init`、`ash`/`sh`、
`--install` 及启动脚本用到的 applet。对自备程序不推断功能和来源。
修改锁定版本应同时更新每个架构的包校验值并运行启动回归。镜像若移除已锁定
版本，构建会报错，不会静默升级；可恢复缓存或使用自备程序。

## 启动职责

内核填充 RAMFS，创建 `/bin/init`、`/bin/sh` 到 `/bin/busybox` 的引导链接，
按现有路径执行 `/bin/init`。BusyBox init 读取以下配置：

- `user/rootfs_busybox/etc/inittab`：执行 rcS、启动登录 shell、回收进程及关机。
- `user/rootfs_busybox/etc/init.d/rcS`：建立可写目录，按二进制实际 applet 集合
  安装 `/bin`、`/sbin`、`/usr/bin`、`/usr/sbin` 的符号链接。
- `user/rootfs_busybox/etc/profile`：设置 PATH、HOME 和串口提示符。

`/proc`、`/sys` 和 `/dev` 由现有内核启动流程挂载。启动脚本不启动 DHCP 客户端、
Telnet 等用户态网络服务；内核网络配置仍沿用实例设置。退出 shell 后由 BusyBox init 重新启动；使用
`poweroff` 或 `reboot` 正常结束。兼容保留 `/bin/poweroff`、`/bin/reboot`。

## 专用配置与测试程序

`auto` 对 NOMMU、ARM32/RISC-V32、发行版引导、媒体、无线、
路由、外置根、只读存储测试及 Native/网络测试配置保留平台 init。
这些配置有独立的平台职责，不能直接用通用 init 替代。可以显式使用
`RAMFS_USERLAND=legacy` 选择平台 init；其内核构建目录与 BusyBox PID 1 模式分离。
平台 init 的 shell 和通用命令也已统一为上游 BusyBox，不再构建 mksh、sbase 或 TLSe。
`RAMFS_USERLAND=busybox` 可强制选择并检查冲突；没有预编译包的 MMU 架构
须同时传 `BUSYBOX_BINARY`。NOMMU 不接受此通用 BusyBox 路径。

`RAMFS_FASTFETCH=1` 可以在 BusyBox 环境中保留 fastfetch；只额外构建 fastfetch
及其所需的 musl，不切回旧 init，也不构建 mksh、sbase 或 TLSe。

普通 BusyBox 环境不包含压力测试二进制。需要 timer/nvme 探针时增加
`RAMFS_DIAGNOSTICS=1`；FPU 和调度测试继续通过 `RAMFS_FPU_TESTS=1`、
`RAMFS_RT_TESTS=1` 单独启用。这些显式选择的本地 C 程序仍需 musl 和对应
交叉工具链，不会因此要求构建 mksh、sbase 或 TLSe。

## FAT、平台 init 与 NOMMU

`user/Makefile` 使用 `tools/packaging/build_busybox.py` 构建原版 BusyBox 1.37.0，
与 A20 专用命令共用静态 musl CRT/libc；NOMMU 使用上游 NOMMU 配置及静态 PIE，shell 为上游 hush（ash 不支持 NOMMU）。
源码 SHA-256 固定在脚本中，下载缓存位于 `build/cache/busybox`，按架构、
编译参数和构建规则隔离输出。生成目录中的 `.busybox.config`、`.busybox.links`、
`.busybox.source.json` 保存配置、applet 集合及来源。

FAT 不支持符号链接，因此只额外保留一份作为 `sh` 的 ELF；普通命令由小型
POSIX 包装脚本调用同目录 BusyBox。平台 init 为它们建立 `/bin/sh` 解释器路径。
RAMFS 中则直接执行 `busybox --install -s` 安装命令链接。关机和重启信号由平台
init 处理，媒体、路由和 Native 等平台启动职责保持可用。

源码版 wget 启用 BusyBox 内置 TLS，不再链接 TLSe，也不探测不存在的 OpenSSL
程序。内置 TLS **不验证服务器证书**；普通 Alpine 静态 RAMFS 包仅提供 HTTP。
需要证书验证时，使用 world 的上游 curl/wget 和 CA 包。验证下载字节成功不能
作为证书认证成功的证据。

mksh/sbase 只在显式 Native ABI 测试中作为按需下载的上游样本，生产构建和镜像
不包含它们。fastfetch 和 musl 继续保留，PPPoE 的公共领域 CHAP-MD5 位于
`user/crypto`。

## 验证

```bash
python3 -m unittest discover -s tools/tests -p test_busybox_ramfs.py
python3 -m unittest discover -s tools/tests -p test_serial_shell_smoke.py
python3 tools/qemu/check_serial_shell.py --arch riscv64 \
  --kernel "$(make -s ARCH=riscv64 RAMFS_USER=1 print-build-dir)/kernel.elf" \
  --memory-mb 256 --busybox --stress-forks 32
```

启动检查验证 init 配置执行、命令链接、ash 管道/循环、外部命令、文件读写及
shell 退出后的自动重启和正常关机。用 `--shutdown reboot` 检查系统重启；此
QEMU 检查不等同于真机验收。接入时修复了 UART FIFO 尾部数据的 poll 就绪判断，
以及阻塞的默认忽略信号被过早丢弃的问题；后者使 BusyBox init 可以通过
`sigtimedwait(SIGCHLD)` 回收并重启 shell。

BusyBox 为 GPL-2.0-only 的独立用户态程序，静态版本也包含 musl。
源码配方链接、精确包版本和架构校验值见锁定文件，包元数据保留在缓存的
`busybox.PKGINFO`。发布包含它的内核或设备镜像时，须一并准备相应许可证、
notice、对应源码及 Alpine 补丁/构建配置；不能只提供 A20OS 仓库链接。

源码 BusyBox 和 PPPoE 相关回归：

```bash
python3 -m unittest tools.tests.test_upstream_userland
python3 tools/tests/check_upstream_downloads.py \
  --busybox user/build/x86_64-ramfs/busybox \
  --kernel /absolute/path/to/platform-init-kernel.elf --out build/check-downloads
# PPPoE 对端及 router 内核的运行方法见 tools/net/tests/test-bsd-pppoe-kvm.py --help
```

源码构建发布时同样需要附带 GPL 许可证、精确归档和构建配置。

LoongArch QEMU 的内核位于高端 RAM，串口回归使用 `--memory-mb 512` 或更大；
256 MiB 只映射低端 RAM，无法容纳该启动地址。

BusyBox 不提供原 sbase 的 `join`、`pathchk`、`sponge`；这些旧构建残留会被清除，
需要时使用 world 中相应上游包。原 `xinstall` 改用标准 `install`。
