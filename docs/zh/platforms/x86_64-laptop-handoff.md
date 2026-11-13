# Lenovo 83EG 实机移植交接

2026-11-10，Legion R7000 APH9，AMD Phoenix，UEFI PJCN07WW。旧版 A20OS
已实机启动到 RAM shell，识别两块 NVMe，并通过内置 ESP 的 `A20LOG/`
落盘 451 条以上 JSONL。Debian 实测确认普通按键来自 ITE USB HID
`048d:c102`。第 5 次启动在实机验证了内置键盘的文本 shell 输入，
摄像头端口只重置一次，内置 ESP 保存了完整的启动记录。第 6 次启动
进一步验证了带源码构建 fastfetch 的镜像和正常重启。

## 第 15 次实机：RSS、重启与锁定灯（2026-11-13）

用户确认运行比第14轮明显改善，但Caps/Num物理灯始终不亮，按钮重启报I/O error，命令行
也不能重启，任务管理器各进程RSS显示16 EB。CURRENT为181d6cb4fa887971ecfab13a13e50bfd，
bootcount15、state ready，1545条连续记录。

RSS确定是procfs格式错误：stat漏掉第21字段itrealvalue占位，RSS工具将本应在第25字段
的rsslim=-1当作第24字段实际RSS；status的非标准Rss同时改为VmRSS。旧输出负对照失败，
修复后49项Rust单元测试通过，C/R节点路由差分200000步通过；真实客体8 MiB映射子进程
的stat/statm/VmRSS读数一致（约27 MiB总RSS），不再呈现64位无符号上限。

246.437 s首次JBD2 poison位于ext4_release_vn回收145002的reap错误分支；440.724 s重启
在文件页回写返回-5，尚未调用固件复位。命令行与GUI共用vfs_system_shutdown，-f也不绕过
内核检查。定位并确定性复现了未锁定文件回写与namespace/orphan更新之间的inode覆盖竞态，
以及相同EOF下per-open旧inode快照覆盖新links/dtime。回写现持metadata_lock并刷新快照，
chmod/chown同锁；C journal在poison后也于缓存变更前拒绝新元数据写入。负对照分别移除锁
和快照刷新均复现覆盖，修复通过。日志未保存首次坏链节点，仍区分已复现竞态与实机根因推断；
新内核补充链预检range/read/missing细节。

实机锁定键与labwc PID96的LED请求正确切换，未出现松开即清零，USB ACK仍不代表灯点亮。
候选显式设置ITE Report Protocol=1并GET_PROTOCOL读回；物理灯效果仍待第16轮，不能以
QEMU普通USB键盘的软件状态代替本机ITE验收。

候选EFI `dfe70d2009af8791df223580bb93ec69a27b96dab2728d0afb4731bcdc3b369e`
使用第15轮实际桌面的修复副本通过两组16 CPU/8 GiB、HPET32真实回绕测试：
USB GUI S5约2.40 s、NVMe命令行reboot -f约1.35 s。客体静置约0.30%/0.32%，
Firefox冷/热DOM约28.94/6.02 s与13.59/5.24 s；均为虚拟机测量。
每案例640轮原orphan压力，加48次并发unlink/576次fsync、unlink后fchmod和数据回读，
最后退出后的fsck=0、state1、无RECOVER、last_orphan0。

实际USB原始备份压缩后约1.43 GiB，完整4 GiB解压SHA已核验，旧备份仍保留。
RAM副本只需4个块/16 KiB修复；原始SHA15691082dce5…，修复SHA93e9b43a83ef…。
修复脚本验证完整分区、补丁、可恢复重复执行与意外改动拒绝。当前等待用户执行物理修复，
尚未切换旧0667017a… EFI。交付时将保留BOOT15.EFI/manifest、计数0000000f及23个旧文件。

证据：`build/evidence/lenovo-83eg-2026-11-13/boot-15-analysis/README.md`；实际安装状态以
usb-readback.json为准。RSS提交9e02617f6、ext4提交cffea8293，原暂存.gitmodules/rtw89保留。
USB初始化仍在既有ITE移植工作区中，候选源码哈希见candidate-source.json；不宣称HEAD包含
全部此前移植。factory在RAM，宿主重启后用本轮restore-factory.py恢复；压缩备份与overlay归档
保留在SSD。下一轮只读采集脚本位于同日boot-16目录。

## 第 14 次启动与第 15 轮交付（2026-11-12）

实机确认WHU-WLAN、永久MAC、关机/重启和触控板可用；静置CPU约50%、Firefox冷启动约30秒、
网页/关机约60秒、锁定灯仍不亮。1540条连续内核记录无晚期跨核时间倒退，触控板约7978次输入，
错误/恢复/丢弃为0。

修复HPET32全局锁竞争并验证每CPU TSC共同时间域，修正CPU8以上GDT CPU ID解码，
去掉BSD调度观察的伪RX和自唤醒，保留真实IRQ及100 Hz轮询。电源重试不再反复sync，
生产关机的全内存/页表审计改为调试开关。Linux syslog READ_ALL编号也已修正。

实际桌面重复测试抓到ext4 orphan初次所有权读取失败，尚未修改元数据就poison整个JBD2事务。
现在此类失败保留持久orphan并延后回收，显式finalize报错保留所有权供重试；开始修改后的
失败仍poison。专用EFI精确注入一次该失败，后续写入、GUI S5与关机后fsck均通过，orphan链清空。
底层单次读取失败的原因尚未定论，已补充分层cache/inode/USB CSW记录。block cache状态机和
extent策略已使用Rust；200000步C→Rust差分和Rust单元测试通过，本次修复采用C事务边界小改动。

最终交付EFI `0667017a96161678e5b55153a1c8b89d82f49409548cf2e39bb351650b96b838`
不含故障注入，使用修复后的真实桌面数据通过16 CPU/8 GiB、HPET32真实回绕两组完整门控：

| 场景 | 客体静置CPU | clock syscall | Firefox冷/热到DOM | 电源退出 |
|---|---:|---:|---:|---:|
| USB + GUI S5 | 0.246% | 742 ns | 25.05 / 5.22 s | 2.00 s |
| NVMe + restart | 0.333% | 717 ns | 12.37 / 4.51 s | 1.25 s |

包含XKB/Rime锁定状态、4 worker/640轮orphan压力，浏览器每轮独立PID且全部任务正常退出。
两次退出后的完整root只读fsck=0，state=1、无RECOVER、last_orphan=0。
这些是虚拟机结果，实机性能待下一轮确认；不能用宿主QEMU halt polling占用代替客体CPU。
旧Firefox夹具只等主PID且可能调用模板内旧助手，旧热启动测量只作历史参考。
旧关机EIO与断点INT3实验仍保留为失败记录，不计入完整门控。

用户已完成实际USB root的7块/28 KiB修复，整个4 GiB回读SHA为80df4e8752e6…，只读fsck通过。
新EFI与manifest已写入指定测试USB，旧版备份为`EFI/A20OS/BOOT14.EFI`和`BOOT14.MANIFEST`。
卸载/只读重挂后完整回读、22个原文件及计数0000000e核对；三个分区均已卸载。
内部SSD及宿主ESP未改写。原4 GiB只读备份保留，磁盘释放约4 GiB；可重建测试镜像移到RAM，
重启后用证据目录restore-factory.py恢复，所有RAM测试overlay压缩归档并核对解压SHA。

Debian灯光报文只在USB路径有效，A20编码与实测一致，但ACK不能证明物理灯点亮。
最终镜像加入各64条上限的Caps/Num源事件与LED请求PID/设备记录，不记录普通按键。
物理灯仍未验收，不宣称已经修复。version.h的0.19/release20.0.19与v0.19一致。
证据与全部哈希：`build/evidence/lenovo-83eg-2026-11-12/boot-14/README.md`，
最终门控见native-acceptance.json，物理修复见desktop-repair-readback.json，交付见usb-readback.json。
第15轮采集脚本在相邻boot-15/目录。

## 从实机与规范得到的结论

| 证据 | 判读与修改 |
|---|---|
| 真机 `ADDRESS_DEVICE ... -17` | xHCI completion code 17 是 **Parameter Error**，USB Transaction Error 是 4。驱动现在单独记录完成码，参数错误时打印 Input Control、Slot 和 EP0 字段。 |
| `06:00.3` HCCPARAMS1 为 `0x0128ffc5` | CSZ=1，Input Control Context 包含 32 字节保留尾部，占满 64 字节。Slot 和 EP0 分别在偏移 64、128。上一版的 32、96 会让硬件读到错位字段。 |
| 旧版 `portsc=0x200603` | 旧日志来自确认 change bits **之前**的快照。PED 是 RW1CS，写 1 会禁用端口。现在只写持久状态和已观察到的 change bits，回读确认 CCS/PED 后才报告 reset 成功。 |
| EP0 的 `tx_info` | Dword 4 低 16 位是 Average TRB Length，EP0 设为 8。Max ESIT Payload Lo 在高 16 位，只有周期端点使用。DCBAA、输入上下文和输出上下文也遵守单页边界。 |
| OVMF 在关闭 i8042、连接 xHCI USB 键盘时返回 `ExitBootServices ... 0x8000000000000002` | loader 曾在取 map key 后写 ESP 日志。现在先验证目标区并落盘日志，再重新取图、验证并退出 Boot Services。失效的 key 只触发重新取图和退出。 |

规范依据见 [xHCI 1.2 表 5-27、6-1、6-91 和 §6.2.5.1](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf)、
[Linux xHCI 上下文实现](https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/xhci-mem.c)和
[UEFI 2.10 ExitBootServices](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html#efi-boot-services-exitbootservices)。

### Linux 上的实际拓扑

- `06:00.3 [1022:15b9]` 是 7 端口 xHCI，Linux 展示 5 个 USB 2 和 2 个
  USB 3 根端口。另有 `06:00.4 [1022:15ba]`、`08:00.4 [1022:15c1]`。
- USB 2 根端口 4 是 ITE `048d:c102`。设备为 Full Speed，EP0 MPS 64，
  接口为 HID Boot Keyboard。Linux 同时绑定了 i8042 的 `atkbd`
  （`FUJ7401 PNP0303`，IRQ1 edge）。枚举只证明两条路径均存在，
  普通按键的来源要由实际输入确认。
- USB 2 根端口 2 是 RTS5411 `0bda:5411`，测试 U 盘
  `346d:5678` 在其下游端口 3，链路为 High Speed。根端口 3 是摄像头，
  端口 5 是蓝牙。USB 3 根集线器另外枚举出一个 RTS5411。
- 加入虚拟 hub 后，旧驱动把 4 口读成 10 口，后续错误控制请求让
  xHCI 等待 Transfer Event 超时。修订版按 USB 2/3 描述符读取端口数，
  用各端口的 `GET_STATUS` 和标准 feature selector 管理电源及 reset。
  xHCI 写入根端口号和 Route String，子设备经各自的 slot 传输。
  FS/LS 设备经过 HS hub 时写入 TT hub slot 和 TT port。

### 旧版真机日志时间线

| boot ID | 观测 |
|---|---|
| `e8ab…` | 内置 ESP 日志 `state=ready`。xHCI 等待 Transfer Event 超时，`sts=0x1019 wanted=32`。 |
| `2b89…` | PS/2 GSI1 强制路由生效，USB 仍失败。 |
| `aac3…` | 改动上下文偏移后，失败提前到 Address Device，完成码 17。 |
| `fe2d…` | 根端口 4 在确认 change bits 前报告 `portsc=0x200603`，Address Device 完成码 17。 |
| `86e1…` | 重试未释放 slot，随后出现 slot 耗尽（完成码 9）。 |
| `e42e894908d51f4472e4d35b5eda7206` | 归档末次内核日志，`state=ready`，403 序号、75,099 字节；根端口 4、MPS 8、完成码 17。 |
| 07:09 旧镜像，08:47 实机启动 | USB 上有 `early-1.jsonl` 与 ESP 的 `RESULT handoff=attempt`。这是 loader 取证，缺少这一轮内核日志。 |

Address Device 保留由控制器分配地址的流程，从 output context 读取地址，
失败后用 Disable Slot 释放 slot。Full-Speed 初始 MPS 仍为 64，随后按描述符
更新。

## 第 2 次实机启动

`CURRENT` 指向 `b8ca28deeb588654a69c982fb560b2c1`，`bootcount=2`，
`state=ready`。日志含 456 条记录，保存在
`build/evidence/lenovo-83eg-2026-11-10/boot-2/A20LOG/`。

- 3.607 s，7 端口 xHCI 的根端口 2 reset 成功，`PORTSC=0xe03`。
  3.631 s，Address Device 成功，slot 和 USB 地址均为 1。
- 3.831 s，首个设备描述符读取等待 Transfer Event 超时。
  等待期间看到了 1 个事件，旧驱动没有记录该事件的内容。此时
  `USBSTS=0x18`，随后软件将该控制器标记为 fatal，扫描跳过了后面的
  根端口，包括 ITE 键盘所在的端口 4。
- 1.986 s，PS/2 IRQ1 确实走了强制 IOAPIC 路由。该路径把触发模式位
  写成了电平触发，Linux 在本机使用边沿触发。修订版按边沿、高有效编程。
- 已修正 EP0 的 Setup、Data、Status TRB 编码。Setup 的 CH 位保留为 0，
  单 TRB 的 Data Stage 也不置 CH。Setup 在其余阶段准备好后才交给控制器。
  等待路径现在接收指向 Setup/Data 的错误完成事件，并在失败时记录请求、
  完成码和 TRB 地址。旧日志不足以确认那 1 个事件的类型和完成码。

## 第 3 次实机启动

U 盘 ESP 的 loader 记录为 `bootcount=3`。A20OS 在 U 盘的 `A20OSLOG`
分区创建了 `boot-a259ff30f05ebe0d427938088b7dae53.jsonl`，证明此前失败的
USB 枚举已经推进到可访问下游存储。该文件和 `CURRENT` 读回为损坏的数据，
先前可读的 `early-2.jsonl` 自偏移 512 字节起也已损坏。损坏副本保存在
`build/evidence/lenovo-83eg-2026-11-10/boot-3/`，本轮内核日志无法用于
判断 HID 绑定或传输结果，USB 写入损坏的原因仍待定位。

回到 Debian 后，用内置键盘按五次 A，`AT Translated Set 2 keyboard`
收到 0 次，`ITE Device(8910) Keyboard` 收到 5 次。Linux 导出的设备报告
描述符显示 Report ID 1 包含修饰键、6 项键值数组和 224 位键值位图，共
37 字节。修订版对 `048d:c102` 的接口 0 保持 USB reset 后的 Report Protocol，
提交 64 字节中断 IN，识别 ID 1 并合并数组和位图中的键值。日志记录设备绑定、
首个报告 ID、帧缓冲控制台状态及传输或重新提交失败。

本轮镜像使用 `LOG_MODE=esp`，内核日志固定写入内置 NVMe 的 ESP
`A20LOG/`。保留专用日志分区的 QEMU 断电测试读回了 228 条 JSONL。
另一次 QEMU 测试关闭 i8042，用 GDB 在 HID 回调注入 ITE 位图报告，
验证了 `echo a20-ok` 回显与 ESP 持久日志。该注入验证了软件解码和
shell 通路，实际 ITE 设备的传输仍待复测。

## 第 4 次实机启动

U 盘启动计数与内置 ESP 的 `CURRENT` 均为 4。内核日志
`boot-79852e6330f6c9652bb468e63037d121.jsonl` 包含 1,313 条有效记录，
`state=ready`，已归档到 `build/evidence/lenovo-83eg-2026-11-10/boot-4/`。

- 3.961 s，ITE 键盘成功绑定，使用 Report Protocol，端点 `0x81`，
  请求 64 字节。30.569 s，收到 Report ID 1，`fbcon=1`，随后解码出
  `KEY_UP` 按下。上方向键走 evdev，文本控制台当前没有对应的字符映射。
  这一轮没有记录字母或回车是否送到控制台。
- `06:00.3` 根端口 3 在这轮日志中重置了 419 次。Debian 的
  `1-3/descriptors` 显示该设备为内置 Chicony 摄像头 `04f2:b7b8`，
  配置描述符 `wTotalLength=879`。旧代码超过 512 字节后释放 slot，
  却以 `0` 返回且未登记设备。后续扫描每隔约 87 ms 重走 reset 和分址。
  日志末尾到 40.945 s，没有 `BOOT REPORT` 记录。
- USB 枚举现在按描述符长度分配缓冲区，EP0 单次最多 4 KiB，xHCI 的
  DMA 缓冲按页对齐。只发布默认 alternate setting 的接口。端口最多尝试
  三次，看到断开后清空失败计数。ITE 把首个可映射按键交给控制台输入接口时
  额外记录一次 `ITE first console input forwarded`，消息不含字符内容。

## 第 5 次实机启动

用户确认内置键盘成功键入。测试 EFI 的 SHA-256 为
`529d50baffddbe6b337698c4ca4589a074a25afa21971fd54e7ff62dc61afca8`。
U 盘和内置 ESP 都记录了 `bootcount=5`，内核 JSONL 共有 485 条连续
记录，`state=ready`。日志、EFI 和实机验收镜像保存在
`build/evidence/lenovo-83eg-2026-11-10/boot-5/`。

| 时间 | 观察 |
|---|---|
| 3.826–3.878 s | 根端口 3 重置一次，摄像头 `04f2:b7b8` 的 879 字节配置枚举为两个默认接口。日志中无 USB 枚举失败或 xHCI 超时。 |
| 3.986 s | ITE 键盘 `048d:c102` 以 Report Protocol 绑定，提交 64 字节中断 IN。 |
| 7.583–7.588 s | 收到 Report ID 1，解码 `KEY_E`，记下 `ITE first console input forwarded`。xHCI 同时记录 `irq=0 poll=1`，本次观察到的是轮询完成。 |
| 14.499 s | 生成 `BOOT REPORT`。持久日志保存报告标记和各条原始记录，报告文本本身受单条记录长度上限限制。 |

本次验收范围是 GOP 文本 shell。日志仍有 `BAT0 _STA`、
I2C-HID 描述符读取和 NVIDIA HDA widget 探测错误。触控板、音频和桌面
输入仍需各自验收。摄像头记录证明 USB 枚举，摄像头视频采集仍待验证。

## 含 fastfetch 的 shell 镜像

笔记本 shell 实例默认启用 `kernel.ramfs_fastfetch`。构建使用父仓库固定的
`user/external/apps/fastfetch` 子模块和现有静态 musl 规则，把程序内嵌为
`/bin/fastfetch`。在 `#` 提示符运行 `fastfetch` 或 `fastfetch --version`。
本次子模块提交为 `b349468a9af7b5721bace373c35637f8808827a7`，
项目版本为 2.67.1。版本号由生成脚本读取子模块的 `CMakeLists.txt`。

QEMU 从 USB hub 下游键盘运行了 `fastfetch --version-raw` 和
`fastfetch`，输出 A20OS、GOP 分辨率、CPU 和内存信息，之后通过 NVMe
读取压力测试并正常关机。已去掉会插入命令输出的 `writev` 调试行。
实机测试 U 盘已写入并读回校验，EFI SHA-256 为
`958858ec464239056b961a9d4d73930be44cd97e6d8f98accfc21023676898f2`。
第 6 次实机启动匹配此 EFI，用户确认键盘和 fastfetch 都正常。
内置 ESP 的 `CURRENT` 为 `bootcount=6`、`state=clean`，
保存 480 条连续记录，共 89,479 字节。7.522 s 记录首个可映射键进入
控制台，14.494 s 生成 `BOOT REPORT`，104.472 s 进入正常重启。
fastfetch 的界面由实机用户确认，内核日志保留的是输入交接与关机证据。
原始资料在 `build/evidence/lenovo-83eg-2026-11-10/boot-6/`。

现场复测的镜像继续使用内置 ESP 持久日志。回 Debian 后通过
`/boot/efi/A20LOG/CURRENT` 的 bootcount 找到当次内核 JSONL。

## 触控板和桌面

第 6 次实机的 `AMDI0010` 控制器在 1.989 s 注册 adapter 0，
下一条 I2C-HID 记录为读取 HID 描述符失败。Debian 上的
`SYNA2BA6:00 06CB:CE78` 对应 `\_SB.I2CA.TPDD`。
Lenovo 固件 `SSDT18` 的 Synaptics 分支给出 7 位地址 `0x2c`、
400 kHz、HID I²C `_DSM` 功能 1 返回寄存器 `0x0020`。
旧版把 uACPI 已解码的资源当作 AML 字节读取，并固定读寄存器
`0x0001`。驱动现在使用解码后的资源与 `_DSM` 返回值。

Debian 导出的 665 字节 HID 报告描述符定义了兼容鼠标报告 2、
触控板报告 3 和 Input Mode 特征报告 4。修订版选择触控模式 3，
为匹配的五个 Finger 集合配置 evdev 槽，并在调度器初始化后启动
轮询线程。日志区分描述符读取、首个报告和首个指针事件。
真实触控板的事件和桌面点击由下一次实机启动验收。
[ACPI 串行总线资源](https://uefi.org/specs/ACPI/6.5/06_Device_Configuration.html#i2c-serial-bus-connection-resource-descriptor) ·
[Linux HID-over-I²C ACPI 描述符查询](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/i2c-hid/i2c-hid-acpi.c)

桌面测试镜像为 `build/x86_64-pc/a20os-laptop-uefi.img`。内核版本与已有的
`v0.19` 对齐，Linux ABI release 为 `20.0.19`。实机候选使用
`LOG_MODE=esp`，先挂载并刷写内置 ESP 日志，再读取 U 盘桌面分区。
早期 QEMU 重复测试曾在桌面负载下遇到 xHCI bulk 超时并导致会话退出。
修订版使用墙钟截止时间，给 bulk 请求 5 秒预算，并记录失败的端点、
长度和 TRB 地址。BOT 缓冲区按大小对齐，块读写的非零 CSW residue
会返回 I/O 错误。

修订版两次通过 QEMU 的 USB hub 下游存储测试。其中一次按实机 U 盘
布局把 XFCE ext4 追加为第 3 分区。两次都渲染了 1280×800 桌面，关闭
i8042 后，USB 键盘仍能打开 XFCE 图形终端并执行命令。Lenovo HID 报告
描述符的主机回放另行验证了两个触点坐标和点击事件。原始证据为
`boot-6/qemu-gui.log`、`boot-6/qemu-gui-appended.log` 和
`boot-6/synaptics-report-replay.log`。该测试覆盖软件路径，
真实 I²C 传输和实机桌面启动仍待验收。

现有测试 U 盘的 ESP 和日志分区保留。部署脚本先保存原 GPT，迁移旧镜像
留下的备份 GPT，再在剩余空间创建桌面分区。读回校验 ext4 后才切换默认
EFI，同时在 ESP 的 `EFI/A20OS/SHELLX64.EFI` 保存第 6 次验收的 shell。
下次从 U 盘启动后，用 `Super+Enter` 打开终端，输入 `echo a20-gui-ok`，
然后用触控板移动指针并点击窗口。内核日志使用 `bootcount=7` 归档。

```sh
tools/a20log show --dir /media/"$USER"/A20OSLOG --boot loader
sudo tools/a20log summary --dir /boot/efi/A20LOG
sudo tools/a20log show --dir /boot/efi/A20LOG --grep ADDRESS_DEVICE
sudo tools/a20log show --dir /boot/efi/A20LOG --grep 048d:c102
sudo tools/a20log show --dir /boot/efi/A20LOG --src USB-HID
sudo tools/a20log show --dir /boot/efi/A20LOG --grep 04f2:b7b8
```

`RESULT handoff=attempt` 只表示 loader 已完成预检并准备最终退出。
看到 `device 048d:c102` 证明枚举，`[USB-HID] keyboard ready` 证明驱动
已绑定。`ITE first interrupt report` 验证传输，
`ITE first console input forwarded` 表示首个可映射按键已交给 tty 输入接口，
命令执行结果用于验收 shell。若仍报参数错误，用新日志中
打印的上下文字段对照 Linux 的 `/sys/kernel/debug/usb/xhci/0000:06:00.3`。
读取 debugfs 应使用 `cat` 或 `hexdump`，这类文件报告的长度常为 0。

## 回归

```sh
git submodule update --init user/external/apps/fastfetch
tools/a20 package x86_64-laptop-shell -- -j8 LOG_MODE=esp
python3 tools/qemu/check_laptop_esp_log.py --keep-log-partition --image build/x86_64-pc/a20os-laptop-shell.img
make smoke-usb-hub-x86_64
python3 tools/qemu/check_laptop_uefi.py --usb-keyboard --internal-esp-log --image /path/to/test-copy.img
python3 tools/qemu/check_laptop_uefi.py --usb-hub --internal-esp-log --fastfetch-version 2.67.1 --image /path/to/test-copy.img
python3 tools/qemu/check_laptop_gui.py --storage usb-hub --image build/x86_64-pc/a20os-laptop-uefi.img
python3 tools/qemu/check_laptop_gui.py --storage usb-hub --usb-keyboard --esp-log-image /path/to/log-disk.img --image build/x86_64-pc/a20os-laptop-uefi.img
python3 -m unittest tools.tests.test_uacpi_namespace
python3 -m unittest tools.tests.test_xhci_endpoint_types tools.tests.test_xhci_firmware_policy
python3 -m unittest tools.tests.test_native_bus_lifetime tools.tests.test_usb_hid_readiness
```

`--usb-keyboard` 和 `--usb-hub` 都禁用 QEMU i8042，并通过 shell 回显验证
实际输入。后者把键盘接到 4 口 hub 的第 3 个下游端口。桌面测试用同样
拓扑连接 USB 存储，挂载 `A20OS distro`，并渲染出 1280×800、694 色
的画面。此 OVMF 版本需要从 UEFI shell 启动该盘的 EFI，测试会自动执行。

此前通过 28 项主机测试，覆盖 CSZ=1、PED 写入语义、本机高速 hub
下游的 Route String，以及全速 hub 继承的 TT 路由。QEMU 下游测试
用全速 hub。Lenovo 控制器及 RTS5411 的高速链路仍需实机验收。

PS/2、直连 USB 和 hub 下游三种 QEMU shell 测试均执行了 1,200 次
NVMe 8 KiB 读取并正常关机，持久日志为 `state=clean`。强制断电
测试保留了 228 条 JSONL。

USB HCD 接口变更后，驱动 ABI 升为 8。使用通用驱动部署的镜像须随内核重建驱动模块。

EP0 修订版通过 29 项主机测试，以及 QEMU 的 PS/2 和 hub 下游键盘 shell 测试。

第 5 次镜像通过长配置描述符和端口失败重连的主机回归。在 QEMU 的 EP0
读回路径回放本机摄像头描述符，879 字节配置只枚举一次，随后生成
`BOOT REPORT`，另一个 USB 端口上的键盘执行了 `echo a20-ok`。
单独注入 ITE 位图报告也得到回显和 `ITE first console input forwarded`。
两个测试的原始日志在 `boot-4/qemu-camera-final.jsonl` 和
`boot-4/qemu-ite-injected.jsonl`。随后第 5 次启动完成实机验收。

### EP0 失败日志回读

使用与测试 U 盘 EFI 校验值相同的镜像，在 QEMU 用 GDB 修改首个
`GET_DESCRIPTOR(Device, 8)` 的完成事件。三种注入均在屏幕
`BOOT REPORT` 中留下 EP0 请求，并在强制终止 QEMU 后从模拟内置 ESP
读回相同记录。

| 注入 | 持久日志 |
|---|---|
| Setup TRB 返回错误 | `cc=5`，事件 TRB 地址指向 Setup |
| 完成事件指向错误的 TRB | `seen=1`，保留最后一个不匹配事件的 pointer、status、control |
| 完成事件对驱动不可见 | `seen=0`，记录请求及控制器和事件环寄存器 |

另一次 QEMU 注入让普通 IRQ1 路由返回 `-EINVAL`，使内核执行强制路由。
IOAPIC 实际配置为 `vec=33 active-hi edge`，PS/2 键盘成功执行
`echo a20-ok`。日志在
`build/evidence/lenovo-83eg-2026-11-10/boot-2/fault-injection/`。
这些实验验证了日志和强制路由分支，Lenovo 上的最终输入仍需实机确认。
若 USB 设备已经绑定但没有任何按键事件，当前日志仍可能无法区分设备没送出
报告和输入中断未送达。若内核在挂载日志介质前卡住，落盘取证取决于启动阶段。
