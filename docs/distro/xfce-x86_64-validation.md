# XFCE x86_64 修复与网页验证（2026-10-11）

## 修复依据

本轮使用 4 CPU、4 GiB、q35/KVM、virtio GPU/键鼠/块设备/网卡，保持
`xfce-x86_64` 的设备配置。桌面组件不再由循环重启或会话看门狗掩盖错误。

| 缺陷 | 修复 | 判别证据 |
| --- | --- | --- |
| `copy_to_user()` 触发 COW 后只刷远端 TLB，本 CPU 用户 VA 仍可读到旧页 | 单页 shootdown 固定当前 CPU，同时刷新本地及远端，并记录本次 generation | fork 后 pipe read 到私有页：旧内核仍读到 `1234`，修复后读到 `decafbad12345678`，子进程仍见原值 |
| 匿名缺页窗口在解锁准备页面后压缩 PFN 数组，丢失 VA 对应关系；快速路径也可覆盖竞争者的 PTE | 保留每个 VA 的候选槽，在叶锁下只安装空 PTE，逐槽返回所有权；swap-in 单独保留替换语义 | 宿主确定性交错覆盖中间页先安装、部分分配失败；guest 四线程首次访问 64 MiB、检查所有字，错误数 0 |
| CPU id 读取与 current slot 读取之间发生迁移 | 对这一对操作关闭中断并恢复原 IRQ 状态 | 原运行捕获 `sched yield: ... not owned by current cpu`；确定性迁移负控取到其他任务，修复取到自身 |
| x86 syscall 入口在公共 handler 采样 CPU-local cause 前开中断 | 架构入口保持关中断，由现有 dispatch-enter hook 在采样后开启 | 修复前 `udevd` 在普通 `cmp` 指令上被报告 instruction page fault、CR2=0；修复后重复启动未再出现 |
| 信号帧覆盖 x86 SysV red zone | 通用 signal 路径调用栈顶 hook，x86 实现在 `kernel/arch` 保留 128 字节 | raw kill 前后检查全部 red zone；旧内核失败，Linux 与修复内核通过 |
| 私有 futex 按物理地址分桶，fork/COW 后 wake 找不到旧桶 | WAIT 保留 private 属性；私有 wait/wake/requeue/wake-op 使用稳定 VA key 并匹配 mm | Pango 队列有任务、序号从 10 变为 11，worker 仍睡在 10；fork/COW guest 回归旧内核 FAIL、新内核 PASS；终端恢复提示符和命令输出 |
| PTY 把 libc termios 的 60 字节布局当作内核 ioctl wire 布局 | 控制台和 PTY 共用内核 termios 与架构 ABI codec | wire buffer 紧邻 PROT_NONE 守护页，master/slave TCGETS/TCSETS 往返；旧内核失败，新内核通过 |

Pango 等待链与其[上游字体查询实现](https://github.com/GNOME/pango/blob/1.56.4/pango/pangofc-fontmap.c)
一致；本轮没有修改上游 Pango 或增加应用名称特判。

## 架构边界

- 删除 `core/trap.c` 中 x86 寄存器、用户堆对象和指令模式诊断特判，保留通用 PTE
  与 trap context 诊断。单步 TF 处理交给 x86 breakpoint hook。
- 架构头由构建 include path 选择，`ARCH_NAME` 由对应架构提供；删除公共页故障代码中
  以 x86/LoongArch 命名的禁用开关，使用统一的页缓存/COW 所有权规则。
- ELF/驱动模块的机器重定位、指令编码、跳板格式、架构符号导出、RISC-V 专有 syscall
  和 PPC termios ABI 位于 `kernel/arch`。通用模块加载器只处理布局、边界、权限和生命周期。
- 板级 MMIO 白名单、设备枚举、TPM 资源、启动时序及中断格式由 `kernel/platform`
  通过 board callbacks 提供；CPU 指令仍位于 `kernel/arch`，通用层不判断板名或 ISA 名。
- `check-arch-boundary` 同时检查编译器 ISA 宏、`CONFIG_*` 架构和板名。现有 vDSO、
  外部代码和独立 STM32 驱动目录仍按原门禁范围管理，并非整个仓库已完成彻底移植。

## 可重跑检查

```sh
python3 -m unittest tools/tests/test_current_migration.py \
  tools/tests/test_fault_window.py tools/tests/test_arch_boundary.py \
  tools/tests/test_tty_arch_abi.py
python3 tools/gates.py check-arch-boundary
python3 tools/gates.py check-mm-lock-model
python3 tools/tests/test_pt_mcs_preempt_window.py
make -j8 smoke-vm-fault-x86_64
make -j8 smoke-futex-stress
make -j8 smoke-drvmod-riscv64
make -j8 smoke-dual-input
make -j8 ARCH=x86_64 BOARD=qemu-virt-x86_64 NR_CPUS=4 \
  PKG_WORLD=xfce PKG_SIZE_MB=4096 image-world
```

`vm_fault_test` 覆盖 red zone、copyout COW、private futex COW、并发匿名缺页；
`pty_termios_test` 覆盖 PTY 的 ioctl wire 边界。二者均可在 dev 镜像 shell 单独运行。
共享 futex、bitset、requeue、PI 与 robust-list 由已有 `futex_stress` 回归覆盖。

内核交叉构建通过：x86_64/QEMU（4 CPU）、riscv64/QEMU（4 CPU）、
aarch64/QEMU（4 CPU）、loongarch64/QEMU（4 CPU）、LoongArch LS2K1000、PPC64LE。
ARM 工具链本机未安装；LoongArch/PPC 的构建通过不代表其 XFCE 交互已验收。
全量 `make check-format` 仍有 132 个既有文件不符合格式；对照本轮开始时
`1e77311e` 的相同文件，新增失败为 0。新建 C/H 文件已按仓库配置格式化。

## 网页与交互

镜像加入 NetSurf、curl，HTML/HTTP/HTTPS 默认打开 NetSurf。正常启动仍用
`tools/a20 run xfce-x86_64`；测试 fixture 不安装到生产 overlay，也不会自动打开网页。

下面只向测试副本注入 fixture：

```sh
mkdir -p build/xfce-web-test
cp --reflink=auto build/images/xfce-x86_64.img build/xfce-web-test/world.img
python3 - <<'PY'
from pathlib import Path
p = Path('build/xfce-web-test/autostart')
s = Path('packages/overlay/xfce/root/.config/labwc/autostart').read_text()
s = s.replace('/usr/lib/a20/start-desktop-components.sh',
              '/usr/lib/a20/xfce-web-smoke.sh >&2 &\n/usr/lib/a20/start-desktop-components.sh')
p.write_text(s)
p.chmod(0o755)
PY
/usr/sbin/debugfs -w -R 'write tools/tests/xfce-web-smoke.sh /usr/lib/a20/xfce-web-smoke.sh' build/xfce-web-test/world.img
/usr/sbin/debugfs -w -R 'rm /root/.config/labwc/autostart' build/xfce-web-test/world.img
/usr/sbin/debugfs -w -R 'write build/xfce-web-test/autostart /root/.config/labwc/autostart' build/xfce-web-test/world.img
qemu-system-x86_64 -machine q35 -accel kvm -smp 4 -m 4G -no-reboot \
  -snapshot -display none -vga none -device virtio-gpu-pci \
  -device virtio-keyboard-pci -device virtio-mouse-pci \
  -drive file=.kernel-build/x86_64-qemu-virt-x86_64-both-dev-smp4-preempt/fat32.img,if=none,format=raw,id=x0 \
  -device virtio-blk-pci,drive=x0 \
  -drive file=build/xfce-web-test/world.img,if=none,format=raw,id=xworld \
  -device virtio-blk-pci,drive=xworld -netdev user,id=net \
  -device virtio-net-pci,netdev=net \
  -kernel .kernel-build/x86_64-qemu-virt-x86_64-both-dev-smp4-preempt/kernel.elf \
  -serial file:build/xfce-web-test/serial.log \
  -monitor unix:build/xfce-web-test/monitor,server=on,wait=off
```

fixture 请求 `http://example.org/` 和 `https://example.org/`，检查响应内容，
HTTPS 保持证书验证，再打开真实浏览器。串口出现三个 `XFCE_WEB: ... PASS`
仅证明请求与进程检查，必须再检查浏览器截图中的 URL、正文以及实际键鼠交互。
使用 monitor 的 `screendump /绝对路径/desktop.ppm` 保存画面；通过 VNC 或
monitor `sendkey`/鼠标事件打开窗口和执行命令。每次测试使用新的 `-snapshot` 运行。

本机证据位于 `build/xfce-debug/`（不纳入源码提交）：

- `old-cow-probe.log` / `cow-probe.log`、`old-pty.log` / `fixed-pty.log`、
  `old-futex.log` 与 `.kernel-build/smoke/vm-fault-x86_64.log` 保存负控/正控。
- `final-web-10.log`、`final-web-11.log` 与对应截图记录最终内核的两次冷启动。
  两轮都通过 HTTP/HTTPS、真实终端 `echo desktop-ok` 输出、终端与浏览器窗口
  切换/关闭、浏览器刷新及鼠标打开浏览器菜单；第 10 轮另取得 Thunar 窗口截图。
  顶栏和 dock 保持可见。每轮观察至少五分钟，无 FATAL/PANIC。
- `final-kernel.elf` 保存所测内核，SHA256 为
  `6cc7d73e59065c677aa51f66711b43ea3bad1096683dcac00fad231a37494547`；生产镜像 `build/images/xfce-x86_64.img`
  已重建，525 个包、87 个 world 条目。`final-qemu-args.json` 保存基础设备参数。

已通过 DNS、TCP、HTTP、TLS、浏览器实际渲染，无需为该场景修改网络栈。
NetSurf 适合轻量网页；本轮只验证上述网页，不据此宣称复杂 JS 网站兼容。
仍可见 xfdesktop 文件图标断言、elogind 未完整启动以及 udev ethtool 超时；
这些已有兼容性缺口需要继续处理。此记录只覆盖 x86_64，不替 RISC-V 桌面签收，
测试 VM 由宿主 monitor 退出，未据此签收 guest 正常关机；
也不包含真实硬件、3D 或音频的验收。额外重复启动 RISC-V 原生 `uinputd`
时发现再次认领后可能收不到 IRQ（设备 used ring 已有事件）；本轮只签收单次认领的
`smoke-dual-input`，重复认领问题保留在 `dual-input-reclaim-failure.log`，不影响使用
内核 evdev 驱动的 x86_64 XFCE 场景。
