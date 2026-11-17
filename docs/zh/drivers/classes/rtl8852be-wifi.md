# RTL8852BE 无线网卡

2026-11-12：Lenovo 83EG 的 Debian 识别内部 PCI 网卡为
`03:00.0 10ec:b852`，子系统 `17aa:4853`，驱动 `rtw89_8852be`。
A20OS 第 13 次实机完成 WHU-WLAN 关联、授权、DHCP 和 GUI 成功状态，用户确认网络可用。
新版移除合成随机 MAC：daemon 从 rtw89 probe 的 efuse 永久地址获取 MAC，在 carrier
开启前通过受权限检查的 ioctl 发布到内核及 BSD/FreeBSD `if_setlladdr`；不再覆盖卡的
硬件地址。新地址通路已通过生产函数回归，实际卡上显示与 DHCP/门户兼容性仍需下一轮复测。

## 镜像与操作

默认笔记本实例启用 `kernel.wifi_rtw89=true`，限定 x86_64、embedded 驱动、
RAM 用户态。构建会从固定 submodule 编译独立的 `rtw89d`，并将固件嵌入程序。
进入 XFCE 前，init 把程序复制到 `/a20` 的 tmpfs 并启动服务；无需重写已有的
4 GiB 桌面分区。`/usr/local/bin/wifi` 指向 `/a20/wifi`。

```sh
git submodule update --init user/external/rtw89
tools/a20 package x86_64-laptop-uefi -- -j8 PREBUILT_WORLD=1 LOG_MODE=esp
```

在桌面双击 **Wi-Fi 网络**，或在应用菜单的网络分类中打开它：

1. 点击“刷新热点”，选择 `WHU-WLAN`，点击“连接”。开放热点不会询问无线密码。
2. 获取地址后，界面检测认证门户；需要认证或无法确认外网时自动打开 Firefox。
3. 在学校网页中输入校园网账号并登录，然后回到 Wi-Fi 窗口点击“检测网络”。
4. 自动打开失败时，可点“打开认证网页”，也可输入学校提供的 HTTP/HTTPS 认证地址。

无法扫描或连接时，窗口内的“诊断日志”可直接查看本轮无线驱动输出，无需打开终端。

武汉大学的[官方上网指南](https://en.whu.edu.cn/info/5301/51031.htm)确认
`WHU-WLAN` 使用 Web 认证。校园网账号与无线密码分开处理。
校园网账号只在正常 Firefox 页面中输入，Wi-Fi 服务不接收或保存网页认证账号。

终端命令仍作为诊断入口：

```sh
wifi scan
wifi connect "热点名称"
wifi status
```

密码在独立提示中输入，不回显、不作为进程参数，也不保存到磁盘。
开放网络直接回车。上游提供 WPA2-PSK / WPA3-SAE 的 station 实现；
企业 EAP、无线凭据保存与自动重连不在此次集成范围。
连接成功要求无线认证完成且 DHCP 脚本配置了 IPv4、路由和 DNS。
`wifi disconnect` 断开并停止 DHCP 续租进程。

## 网页认证

`wifi-gui` 使用现有的 Python / GTK 3，操作通过本地 socket 的结构化接口发送，
SSID 按原始字节保留；不把热点名称或无线密码拼入 shell 命令。
网页认证与无线关联、DHCP 分开显示，获得 IP 不等于已经联网。

- DHCP 请求 [RFC 8910 的 option 114](https://www.rfc-editor.org/rfc/rfc8910.html)。
  若网络提供 HTTPS CAPPORT API，则按 [RFC 8908](https://www.rfc-editor.org/rfc/rfc8908.html)
  检查 `captive` 和 `user-portal-url`，TLS 验证保持启用。
- 兼容传统 HTTP 重定向、200 页面及 511 认证响应；HTML/JavaScript 的跳转交给
  完整 Firefox 处理，支持登录表单、Cookie、跨域跳转和页面脚本。
- 联网检查要求 HTTP 与经过证书验证的 HTTPS 探测均返回预期空 204。
  主探测使用 `connect.rom.miui.com`，失败时再试 `connectivitycheck.gstatic.com`；
  DNS、超时、证书错误都不会被标记为联网成功。检查失败时保留手动认证入口。
- 浏览器使用正常用户配置，不自动提交学校账号，不关闭证书校验，不启用全局 HTTPS 降级。

## 实现与边界

- 内核 `rtw89_broker.c` 只提供 PCI 配置、BAR2 映射、32 位一致性 DMA、
  Ethernet 帧队列及认证后的 carrier。无线协议与固件初始化在独立进程内运行。
  映射前检查 PCI PM 状态，必要时转为 D0 并恢复 BAR；D3hot 转换等待参考
  [Linux PCI 电源管理实现](https://github.com/torvalds/linux/blob/v6.12/drivers/pci/pci.c)。
- `/dev/rtw89` 需要初始 user namespace 的 `CAP_SYS_RAWIO`，同一时刻仅一个
  地址空间可拥有。映射不继承到 fork；释放 DMA 前撤销映射。
  bus master 的启停检查 PCI 命令回读；停止失败保留设备绑定。解除 DMA 映射失败
  时保留内存、token 和映射记录；最后一次 close 的解除映射失败保留 MM 引用供停止重试。
  服务异常退出且仍有 DMA 时，尝试禁用 bus master、隔离内存并拒绝重新接管，需重启。
- 首轮使用每毫秒轮询设备中断状态，PCI INTx 禁用。这是正确性优先的移植路径，
  实机吞吐、CPU 开销、功耗和休眠恢复尚未测量。
- 原有 BSD 网络栈接收解封装后的 Ethernet 帧。接口名按 MAC 查询，不假定无线卡
  一定是 `eth0`。DHCP 使用桌面已有的 BusyBox `udhcpc`。

## 日志和验收

`/var/log/a20/wifi.log` 保存当前无线服务输出；上一轮为 `wifi-prev.log`。
驱动的 `IOLog` 同时写入 `/dev/kmsg`，沿用内置 ESP `A20LOG` 的持久记录。
启动失败时先查看日志，不要反复手工运行第二个服务进程。

```sh
cat /var/log/a20/wifi.log
wifi status
ip addr
ip route
cat /etc/resolv.conf
ping -c 3 1.1.1.1
curl -I --max-time 20 https://example.com
```

实机通过标准：真实热点出现在扫描结果，输入密码后 `authorized=1`、`ip` 非
`none`，网关路由与 DNS 有效，HTTPS 请求成功。仅有 PCI 枚举、固件下载或
QEMU 自检均不能证明无线联网。后续采集日志应保留上述状态和失败阶段，勿收集密码。

2026-11-17 的 broker 定向检查覆盖停止失败保留绑定、解除映射失败保留 DMA/MM、
关闭准入、正常归还后重新绑定与异常遗留隔离。两个宿主方法通过，启用 Wi-Fi 的
x86_64 embedded 配置编译 broker 对象通过；PCI、MM/VFS 与锁由模型提供，未操作
实体网卡，不作为无线硬件停止或崩溃后恢复证据。详见[驱动现状](../../status/current/drivers-implementation-status.md#rtw89-broker-停止与映射失败保留2026-11-17)。

图形流程的模拟认证服务器已验证：点击连接开放热点时发送空无线密码，弹出真实
Firefox，经过 HTTP 302、JavaScript 表单处理、POST、Cookie 和跳转，登录后
回到 GUI 重新检测外网成功。该测试的无线关联由测试 socket 模拟，认证页是本地
测试页，**不代表已经完成 WHU-WLAN 实机认证**。测试 fixture 不包含在生产镜像中。
记录在 `build/evidence/lenovo-83eg-2026-11-11/wifi-gui/`。

已完成的模拟验证：16 vCPU / 40 GiB、xHCI hub 下游 USB 存储、USB 键盘、
XFCE 1280×800，以及桌面内 `/a20/rtw89d --self-test`。同一桌面通过 QEMU
E1000 获取 `10.0.2.15` 租约，执行本次 `wifi-dhcp` 后读回默认路由与 DNS。
与实机 U 盘相同的三分区布局也通过上述检查，并用 `curl` 完成 DNS 和
`https://example.com` 的证书校验及 HTTPS 请求。
这是用户态集成及网络配置验证；QEMU 未模拟 RTL8852BE。

### 第 9 次实机初始化修复

本机 PCI 枚举与 BAR2=`a1a00000` 有效，发现 broker 对 usercopy 的判断使用了错误约定：
A20OS 成功返回复制字节数，原 INFO 路径却将成功当成 EFAULT；config/DMA/link 也受影响。
所有相关判断改为仅负值失败。主机回归编译生产 ioctl，使用真实字节数约定覆盖这些操作，
并以旧判断为失败负对照。此修复解决初始化误报地址错误，不能替代实际无线关联验收。

无线服务现在把打开设备/INFO、BAR 映射、radio probe、MAC、启动结果和 errno 镜像
到内置 ESP 的 A20LOG；GUI 记录按钮动作以便与输入故障对齐，不记录认证凭据。
证据目录 `build/evidence/lenovo-83eg-2026-11-11/boot-9/`。

### 第 11 次实机 DMA 分配修复

INFO、BAR2 与固件阶段成功，txch 8 的 txwd ring 申请返回 ENOMEM。
原 DMA 分配通过普通 PFA 分配/释放重试；大内存下 LIFO buddy 会反复返回同一
4 GiB 以上的块。现用 `pfa_alloc_max` 按设备 mask 直接选择物理内存，保留
分配后范围校验，失败额外记录请求大小、已用额度、预算和 mask。
物理几何回归含旧代码负对照，内核启动自检同时保留 32 个 64 KiB 的 32 位缓冲区。
该修复仍需 RTL8852BE 实机初始化、扫描、关联和 WHU-WLAN 网页认证验收。

### 第 12 次实机：无线关联通过，修复控制连接与重复连接

本轮 probe/MAC/start 成功，扫描得到 52 个真实热点，WHU-WLAN 在 5220 MHz
完成开放认证与 802.11ax 关联，carrier=1，DHCP 返回 0。网页认证和外网仍未验收。
后续 GUI 状态操作快速失败，重复关联返回 -16。

发现公共 accept 事务错误继承监听 socket 的 O_NONBLOCK，与 Linux 接口约定不符；
服务采用非阻塞监听，但期望新连接上的请求读取是阻塞的。修复模式选择，并在 fd 安装
时保留 SOCK_CLOEXEC。旧代码负对照失败，新模式/域/flags 矩阵测试通过。
相同 SSID 的重复连接改为复用授权与地址；需要重试 DHCP 时保留无线关联。
服务与 GUI 增加状态、失败阶段、异常类型及 errno 诊断。旧实机日志未保存异常详情，
不能把每次快速失败均断言为同一种原因。详见 `boot-12/README.md` 证据记录。

## 固定来源

- [AirPort_RTW89](https://github.com/FranciumOS-org/AirPort_RTW89)，gitlink
  `90b6cfc5a69c271688fad188857cfaeb58053dc5`。上游树保持原样；构建目录中仅裁剪
  glue 的 PCI 驱动表至 8852BE，编译其 C 平台接口、Realtek 驱动及 station/crypto。
- linux-firmware commit `d947e4e8e314e9254a1242dc1a5d9cede2cce33d` 的
  `rtw89/rtw8852b_fw-2.bin`，SHA-256
  `6ff6aeb56bb5b85cba333b755f8cc1dbfe9ff6e5b16e39f6ba01ce7f1eaa7714`。
  下载后及每次构建都核验；缓存位于 `build/downloads/`。
- `rtw89d` 含 GPL-2.0 代码；平台适配与 CLI 为 BSD-3-Clause，内核 transport
  为 Apache-2.0。程序、上游版权及固件条款合并为镜像 `/a20/rtw89-notices`；
  固定源码、构建命令与 firmware hash 记录于用户态 `rtw89-build/build.json`。
