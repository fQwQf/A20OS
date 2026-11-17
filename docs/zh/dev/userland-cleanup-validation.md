# 用户态依赖清理与验证（2026-11-15）

生产构建保留 musl 与 fastfetch，移除 mksh、sbase、TLSe 的 submodule 和构建依赖。
普通精简 MMU RAMFS 使用固定版本的 Alpine 静态 BusyBox；FAT、平台 init 和
NOMMU 使用 SHA-256 固定的上游 BusyBox 1.37.0 源码，与本地命令共用 musl。
MMU shell 为 ash，NOMMU 为上游支持的 hush。构建说明见
[BusyBox 用户态](busybox-ramfs.md)。

PPPoE 仍承担 A20 BSD TUN 的平台适配；CHAP 只链接 `user/crypto/md5.c` 的
公共领域实现，不再依赖 sbase libutil。删除自有 wget、ls、ps、poweroff、reboot，
这些命令由 BusyBox 提供。FAT 的 applet 包装脚本定位同目录 BusyBox，保留参数
边界；`sh` 为实际 ELF。清理旧构建残留，原 `xinstall` 改用 `install`。
BusyBox 不提供的 `join`、`pathchk`、`sponge` 不再进入基础用户态。

mksh/sbase 仅在显式 Native ABI 回归中作为上游应用样本，按固定提交获取到
`build/cache/test-sources`。生产构建不获取它们。对应 mlibc 样本已经成功编译，
本轮没有把其 guest 运行计为通过。

## 实际验证

以下为本地 QEMU/宿主结果，不等同于真板验收。日志及 JSON 原始结果位于
`build/tmp/userland-cleanup/`，该目录属于忽略的构建产物。

| 路径 | 结果 | 覆盖 |
| --- | --- | --- |
| 宿主回归 | 62 项通过 | 构建身份、无退役生产依赖、包装脚本、MD5 向量、RAMFS 配置、串口 oracle、UART、NOMMU usercopy 等 |
| x86_64 MMU RAMFS | 通过 | BusyBox PID 1、ash 管道/循环、命令链接、32 个子进程、文件操作、shell 重启、关机 |
| AArch64 MMU RAMFS | 通过 | 同上，256 MiB QEMU，使用 kernel.bin |
| LoongArch64 MMU RAMFS | 通过 | 同上，512 MiB QEMU；256 MiB 仅有低端 RAM，不能容纳此内核高端启动地址 |
| RISC-V64 MMU RAMFS + fastfetch | 通过 | 同上，并执行 fastfetch 版本与 OS/Kernel/Memory 输出 |
| RISC-V64 NOMMU RAMFS | 通过 | hush、32 个外部子进程、子 shell、文件操作、正常关机，256 MiB QEMU |
| x86_64 平台 init | 通过 | 上游 shell、文件操作、正常关机及 reboot |
| x86_64 完整源码用户态及 FAT32 | 构建、运行通过 | FAT 包装脚本、install、wget、fastfetch、32 个子进程、文件操作、同步、关机 |
| ARM32 源码用户态 | 编译通过 | 最新 BusyBox 配置、sh、平台 init、PPPoE；不计为真板或 guest 运行通过 |
| HTTP/HTTPS | 宿主及 x86_64 guest 通过 | 本地服务、重定向、404 失败、二进制下载 SHA-256；JSON 在 downloads-final/result.json |
| PPPoE PAP | 真实 Linux 对端通过 | IPCP、LAN NAPT、LCP 静默超时、断线重拨换地址、错误密码与 TUN 回收 |
| PPPoE CHAP 双栈 | 真实 Linux 对端通过 | CHAP-MD5、IPv4 NAPT、IPv6CP、IPv6 转发、IPCP DNS、重拨恢复、错误密码、抓包协议检查 |

PPPoE 对端使用 Linux `pppd` / `pppoe-server`，两台 KVM QEMU 通过虚拟链路连接。
PAP 与 CHAP 清单位于 `pppoe-router-pap/manifest.json` 和
`pppoe-router-chap/manifest.json`，包含参数、内核/对端镜像摘要和逐项结果。
测试内核必须设置 `NET_ROLE=router`，普通主机网络配置不启用转发。

## 验证发现并修复的兼容问题

- RISC-V NOMMU 使用独立信号返回映射，避免 SIGCHLD 交付时尝试升级平坦栈页
  权限而终止 hush；补齐 NOMMU 页表销毁空实现和 kexec 编译条件。
- 轮询式 UART 返回空 readiness 事件源，使 poll/select 使用现有定时轮询，
  避免等待一个没有 IRQ 生产者的队列；LoongArch 串口复测通过。
- 平台 init 在关闭 FAT 文件系统前等待 shell/telnet 子进程退出，避免解释器
  持有脚本文件句柄而导致 shutdown 返回 EBUSY；关机、重启日志保持兼容。
- 源码 BusyBox 关闭独立用户态中不存在的 OpenSSL helper，直接使用上游内置
  TLS，避免上游 1.37 在 vfork 后 exec 失败时调用 exit 污染 musl 退出锁。
  BusyBox 与 musl 源码均未为此打补丁。

## 下载能力边界

源码 BusyBox 内置 TLS **不校验服务器证书**；测试中的自签证书下载成功只验证
传输及内容完整，不代表服务器身份认证，设置 SSL_CERT_FILE 也不提供该能力。
普通精简 RAMFS 的 Alpine busybox-static 未启用 HTTPS。需要证书验证的下载，
使用 world 中上游 curl/wget 与 CA 包。

## 复现入口

```sh
python3 -m unittest tools.tests.test_stamps_clean tools.tests.test_board_shell_profiles \
  tools.tests.test_busybox_ramfs tools.tests.test_pc_package_paths \
  tools.tests.test_serial_shell_smoke tools.tests.test_upstream_userland \
  tools.tests.test_uart_readiness tools.tests.test_uart_tx \
  tools.tests.test_musl_build_identity tools.tests.test_pkg_make_dependencies \
  tools.tests.test_usercopy_nommu_host tools.tests.test_riscv_hyp_nommu

make ARCH=riscv64 RAMFS_USER=1 RAMFS_FASTFETCH=1 -j4 kernel-only
python3 tools/qemu/check_serial_shell.py --arch riscv64 --busybox --stress-forks 32 \
  --memory-mb 256 --kernel "$(make -s ARCH=riscv64 RAMFS_USER=1 RAMFS_FASTFETCH=1 print-build-dir)/kernel.elf"

make ARCH=riscv64 NOMMU=1 RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded -j4 kernel-only
python3 tools/qemu/check_serial_shell.py --arch riscv64 --stress-forks 32 \
  --memory-mb 256 --kernel "$(make -s ARCH=riscv64 NOMMU=1 RAMFS_USER=1 DRIVER_DEPLOYMENT=embedded print-build-dir)/kernel.elf"

make ARCH=x86_64 RAMFS_USER=1 RAMFS_USERLAND=legacy RAMFS_NET_TESTS=1 \
  DRIVER_DEPLOYMENT=embedded NET_ROLE=router -j4 kernel-only
# 本地 HTTP/TLS 服务与真实 Linux PPPoE 对端的参数：
python3 tools/tests/check_upstream_downloads.py --help
python3 tools/net/tests/build-pppoe-peer.py --help
python3 tools/net/tests/test-bsd-pppoe-kvm.py --help
```

## 日常入口补充回归（2026-11-15）

前述 RAMFS/x86_64 源码验证没有覆盖 `tools/a20 run qemu-riscv64` 的默认
RISC-V64 源码用户态路径。清理后重新构建复现了 BusyBox `free.c` 的 sysinfo
声明缺失：默认 `riscv64-unknown-elf-gcc` 不定义 `__linux__`，而上游 BusyBox
据此选择 Linux 头文件与 OS 行为。现将 Linux ABI 平台定义显式传入 BusyBox，
并把用户 Makefile 加入二进制依赖，保证配方修改后重新选择正确缓存。

实际执行原命令 `tools/a20 run qemu-riscv64`，已验证进入 ash、BusyBox free、
fastfetch、32 次外部子进程、文件操作、同步和正常关机。新增裸机编译器下的
Linux 头文件编译探针，避免只验证预编译 RAMFS 或 Linux 交叉编译器路径。

`make clean` 原先只删除 `build/tmp` 和当前架构用户态目录，现清理所有架构
用户态、内核及 `build/` 中的生成产物，保留 cache/toolchains/downloads；
`make distclean` 删除剩余缓存。隔离目录中实际执行生产 Make 规则，验证跨架构
清理、隐藏文件、镜像、缓存保留/移除和符号链接目标保护；删除错误不再被吞掉。
