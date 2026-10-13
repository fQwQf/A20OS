# XFCE x86_64 修复与网页验证（2026-10-12）

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
make -j8 ARCH=x86_64 BOARD=qemu-virt-x86_64 ABI=both NR_CPUS=4 \
  CONFIG_KERNEL_PREEMPT=1 PKG_WORLD=xfce PKG_SIZE_MB=4096 image-world
```

`vm_fault_test` 覆盖 red zone、copyout COW、private futex COW、并发匿名缺页；
`pty_termios_test` 覆盖 PTY 的 ioctl wire 边界。二者均可在 dev 镜像 shell 单独运行。
共享 futex、bitset、requeue、PI 与 robust-list 由已有 `futex_stress` 回归覆盖。

内核交叉构建通过：x86_64/QEMU（4 CPU）、riscv64/QEMU（4 CPU）、
aarch64/QEMU（4 CPU）、loongarch64/QEMU（4 CPU）、LoongArch LS2K1000、PPC64LE。
上述为前期构建记录；本轮追加的交叉构建另见后文。构建通过不代表对应架构 XFCE 交互已验收。
全量 `make check-format` 仍有 132 个既有文件不符合格式；对照本轮开始时
`1e77311e` 的相同文件，新增失败为 0。新建 C/H 文件已按仓库配置格式化。

## 网页与交互

此前 NetSurf 的 example.org 截图只验证了轻量页面，不能解释用户报告的
Bing 空白问题。现在 XFCE world 加入 Firefox，HTML/HTTP/HTTPS 默认使用
`firefox.desktop`；NetSurf 保留作轻量浏览器。镜像采用 Alpine Firefox
151.0.3，正常启动仍用 `tools/a20 run xfce-x86_64`。

Firefox 的 forkserver 会为没有文件描述符的回复附带空 SCM_RIGHTS 头。
此前内核错误地以 EINVAL 拒绝，导致 `failed to send a reply message` 断言；
修复接受空列表，继续解析后续控制消息，详见
[Linux Unix ancillary ABI](../linux-unix-ancillary.md)。MM 的 fork shootdown、
用户复制生命周期和回收提示数据保留修复另见
[MM 并发约束](../mm-fork-mprotect-cow.md)。另修复工作线程的 Unix 凭据、
SO_PEERPIDFD 选项号和 CLOEXEC，以及
[线程组结束时的 SIGCHLD 通知](../linux-child-exit-notification.md)；后者修复
已退出的 xfconf-query 仍让 shell 等待的问题。所有修复都使用通用接口。

### 重跑 fixture

测试 fixture 只注入镜像副本，不安装到生产 overlay。它保留 TLS 证书验证，
用 curl 检查 HTTP/HTTPS，再通过 Firefox 本机 Marionette 接口读取真实网页
DOM，验证 Bing 首页、实际输入搜索词并提交、搜索结果、刷新、开关标签页
以及五分钟内容进程存活。启动浏览器前还验证 XFCE 配置的写入、读回和删除。
`XFCE_WEB: Firefox PASS` 必须出现在串口中；进程存在不算网页通过。
Marionette 仅在测试中由 `--marionette` 开启。

```sh
mkdir -p build/xfce-web-test
cp --reflink=auto build/images/xfce-x86_64.img build/xfce-web-test/world.img
python3 - <<'PYCODE'
from pathlib import Path
p = Path('build/xfce-web-test/autostart')
s = Path('packages/overlay/xfce/root/.config/labwc/autostart').read_text()
p.write_text('/usr/lib/a20/xfce-web-smoke.sh >/dev/tty0 2>&1 &\n' + s)
p.chmod(0o755)
PYCODE
/usr/sbin/debugfs -w -R 'write tools/tests/xfce-web-smoke.sh /usr/lib/a20/xfce-web-smoke.sh' build/xfce-web-test/world.img
/usr/sbin/debugfs -w -R 'write tools/tests/xfce-firefox-webdriver.py /usr/lib/a20/xfce-firefox-webdriver.py' build/xfce-web-test/world.img
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

还需通过 VNC 或 monitor 键鼠事件检查终端输入、窗口切换和浏览器画面；
`screendump /绝对路径/desktop.ppm` 可保存截图。每轮使用新的 `-snapshot`。

### 本轮证据与边界

四核 KVM、4 GiB、抢占内核，两轮独立 `-snapshot` 冷启动均完整通过：

- `build/browser-followup/firefox-final-{1,2}.log`：配置写入/读回/删除、curl
  HTTP/HTTPS、Firefox HTTP/HTTPS DOM、Bing 输入搜索和结果、刷新、标签页操作、
  五次每分钟 DOM 查询，以及最终 desktop and browser PASS。
- `firefox-final-bing.png`、`firefox-final-manual-bing.png`：搜索结果与完整背景图首页；
  `firefox-final-terminal-ok.png`：终端键盘输入、配置命令返回及窗口切换。
  截图均位于同一目录；第二轮画面另存 `firefox-final-2-pass.png`。
- 使用 `sigchld-group-kernel.elf`，SHA-256
  `ae51ba8400f5f977c3a18cbd7e6169b5eeddeb04b17816d3665c4aa379f6b668`。
  最后一次仅将 CLD_DUMPED 字面量命名的构建，`.text` SHA-256 与它完全一致。
- world 为 `build/browser-followup/minecraft-image/xfce-x86_64.img` 的测试副本，
  532 个包、88 项 world 入口，含 Firefox 151.0.3 与已校验的 Minecraft 数据。
  原始 world SHA-256：
  `707a4e765b418655e784a08220fa0cdca84d02d0035d6e3e9980d0cb3a09682d`。
  测试副本只额外注入 fixture/autostart；生产 overlay 不开启 Marionette。

Firefox ESR 140 的 musl 堆断言在内存修复后仍可复现；它不作为本轮验收的
默认浏览器。Firefox 151 使用包自带的 Scudo，未注入分配器替换或 preload。
HTTP/3 的 neqo 套接字选项仍报告 EOPNOTSUPP，浏览器使用 TCP/TLS 回退；
这不构成 QUIC 支持验收。仍可见 xfdesktop 文件图标断言、elogind 和 udev
ethtool 的兼容性缺口。本记录只覆盖 QEMU x86_64 桌面，不代表真实硬件、
其他架构桌面、音频或 Firefox WebGL 的验收。Minecraft 的 virgl 验证见 [Minecraft](minecraft.md)。


### 图形修复后的 Firefox 回归

GPU 资源、PRIME 共享和 fence 修复后，再用生产镜像副本各完成一次冷启动：

- `build/browser-followup/firefox-updated-2d.log`：普通 virtio GPU，自动选择 pixman。
- `build/browser-followup/firefox-updated-3d.log`：virgl GPU，自动选择 GLES。

两轮均完整通过配置往返、curl HTTP/HTTPS、Firefox 网页 DOM、Bing 搜索与刷新、
标签页操作和五分钟稳定性检查，最终包含 `XFCE_WEB: desktop and browser PASS`。
对应画面为同目录 `firefox-updated-{2d,3d}-bing.png`。
此处内核 SHA-256 为
`965962d4520347257f4ab19da083fabbb9e36613c1d0a2c5ecbe6149a81a67a4`。
随后另修复 [SIOCGIFCONF](../linux-interface-enumeration.md) 的输出语义，
该修复通过 x86_64/RISC-V 原生回归；它不改变浏览器套接字的数据传输路径。

会话通过 `VIRTGPU_GETPARAM(3D_FEATURES)` 选择渲染器，不判断架构或应用名；
`A20_RENDERER` 可显式覆盖。用户默认的普通 XFCE 实例仍能使用 2D GPU，
Minecraft 专用实例开启 virgl。详见
[GPU 共享与同步](../graphics/virtgpu-sharing-and-sync.md)。
