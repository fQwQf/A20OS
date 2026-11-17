# Lenovo 83EG 实机移植交接

2026-11-10，Legion R7000 APH9，AMD Phoenix，UEFI PJCN07WW。旧版 A20OS
已实机启动到 RAM shell，识别两块 NVMe，并通过内置 ESP 的 `A20LOG/`
落盘 451 条以上 JSONL。Debian 实测确认普通按键来自 ITE USB HID
`048d:c102`。第 5 次启动在实机验证了内置键盘的文本 shell 输入，
摄像头端口只重置一次，内置 ESP 保存了完整的启动记录。第 6 次启动
进一步验证了带源码构建 fastfetch 的镜像和正常重启。


## 第19轮实机检查（2026-11-14）

用户确认整体可用但卡顿。实际EFI与0.20交付哈希一致，内置ESP保存1668条连续JSONL，
state clean；宿主只读fsck=0，RECOVER=0、last_orphan=0。16/16配置CPU上线，
I²C-HID收到9157个有效报告，驱动传输errors/drops/recoveries均0，未见USB超时或内核panic。
WHU-WLAN关联、DHCP及永久MAC发布成功；日志不能单独验收网页门户认证。

这不是完整桌面通过：libinput至少5次跳点后限流（第18轮也有），Firefox有8次IBus按键
请求超时，LibreOffice在空Java配置XML告警后抛异常并自发SIGABRT，udev的两个ethtool
辅助管道有重复超时终止记录。后两者根因尚未确定，不先归咎介质或应用配置。
桌面使用Pixman，Xwayland回退软件渲染；不能把所有卡顿等同于U盘的正常表现。
重启请求至最终系统调用114.199 s，最终sync 101295 ms，根分区100932 ms；最后JBD2
ordered=88043 ms、finish=12716 ms，全部sync返回0。慢写回仍未解决。

本轮仅只读采集并分析，未改测试EFI、未修盘或回放journal。后续应定位输入法/udev阻塞、
写回与flush时延，并单独回归LibreOffice及触控板事件质量。原始证据和可复算摘要见
`build/evidence/lenovo-83eg-2026-11-14/boot-19-analysis/README.md`、`acceptance.json`。

## 第19轮交付（2026-11-14，待实机验收）

用户授权更新测试U盘。已从提交 `388197124dc6909906a8c9d739a394217dc87fa9` 独立构建0.20版，
EFI SHA256为 `8580a520a0cd6476e3fef430784ce3e2bda763b65e8a18bf4489bacbe5733421`，已写入并清缓存、只读重挂回读一致。
仅更新ESP内EFI/清单；GPT不变，桌面分区不写入，前后只读fsck=0，其他30个原文件哈希不变。
计数仍为00000012（18），下一轮应为19；三分区均已卸载，宿主未重启、内置SSD未改写。
旧版保存在 `EFI/A20OS/BOOT18.EFI` 和对应manifest，独立shell仍保留。

独立源码通过44项PC、371项固件/驱动及sleep ABI测试；同布局USB虚拟副本通过16 CPU/8 GiB
桌面输入、rtw89服务自检、OpenRC监督和图形S5，退出后fsck干净。这些不替代实机验收，
也不表示第18轮约105秒的最终同步已解决。
实物回读、完整ESP备份、构建与虚拟验证见
`build/evidence/lenovo-83eg-2026-11-14/boot-19-deploy/README.md`。

## 第18轮验收与驱动整理（2026-11-13）

用户已确认桌面使用良好。内置 ESP 的第18轮记录共1320条，序号连续、state clean；
触控板收到6566个有效报告，错误、恢复及丢帧计数均为0。该轮验收对应 EFI
`669f88613c20cfa048c17d2d67f38cf2875d43336889e9d957938e6dfd249a7f`。

关机性能仍有明确限制：重启请求至最终系统调用约118.89 s，最终同步105332 ms，
其中根分区104999 ms，JBD2 ordered阶段88125 ms。不能把QEMU约10 s电源流程当作
该U盘实机已优化到同样速度。继续保留分阶段诊断，不将长时间同步写成已解决。

本次整理把 NVMe/PS2 实现移入对应驱动类别、保持模块入口轻量；拆开 I²C 注册表、
GPIO输入适配、HID传输和Synaptics差异，使用每设备工作队列与引用关系；桌面会话工具
不再依赖Wi-Fi打包。成熟无线实现继续位于独立用户进程，内核仅负责受控硬件传输。
复用步骤、资源及锁契约、部署边界和验证命令见驱动指南 `guide/x86-hardware-reuse.md`。

整理后的代码有新的生命周期及打包变更，不能沿用旧EFI的实机通过结论。
本轮不改测试U盘、不重启宿主、不划分SSD；锁定灯继续按用户决定停止排查。
原第17轮证据目录已不在当前工作区，第18轮采集和本轮验证记录保存在
`build/evidence/lenovo-83eg-2026-11-13/driver-consolidation/`。

## 第17轮验收与关机优化（2026-11-13）

实机已确认新版 OpenRC 桌面、网络等使用正常，但本轮图形重启请求至最终 reboot 系统调用
入口约101.27 s。1490条内核记录连续、state clean，U盘根分区只读fsck=0。服务退出约9 s，
之后约92 s仍在最终信号/同步段，尚未进入固件复位；不能直接归因于ACPI超时。

排查发现 `VFS_MOUNT_DETACHED=2` 与 Linux `MS_NOSUID=2` 冲突：OpenRC 更新 `/run`
属性后，路径查找绕过tmpfs，把运行时文件及socket标记落到ext4。重用本轮实际根分区测试时
D-Bus因此Address in use。内核已将detached改为独立状态，remount保留namespace ownership；
生产C adapter/Rust matcher及旧位判断负对照、200000步C/Rust差分通过。

同时合并连续JBD2 payload写入，128 KiB封顶；187个连续4 KiB块由187次写变为6次。
不删除任何flush屏障，错误仍阻止后续提交；补充sync/JBD2分阶段耗时。图形工作区在DRM真正
交还显示时恢复文本控制台，关闭render/probe文件不会抢占其他输出客户端。

| 最终候选验证（16 CPU / 8 GiB） | 正常电源动作耗时 | 退出后检查 |
|---|---:|---|
| USB桌面，图形S5 | 9.876 s | fsck=0，clean，RECOVER=0，last_orphan=0 |
| USB桌面，普通reboot | 9.920 s | 同上 |
| NVMe桌面，图形restart | 9.659 s | 同上 |

三轮均使用实际第17轮根分区的只读副本及同一EFI，确认 `/run` 仍在RAM、服务正常监督，
留下64个文件/16 MiB待同步数据再退出，并保存恢复后的文本控制台截图。
这些是QEMU结果，尚不能保证这台机器的U盘实测关机时间。新镜像可由永久压缩档案在RAM中
完整重建，已经重建并核对整个镜像SHA256。

证据：`build/evidence/lenovo-83eg-2026-11-13/boot-17-analysis/README.md`、
`native-acceptance.json`、`candidate-source.json`。本轮核心提交 `cb144381f`（挂载状态）及
`be1188cfd`（日志批量写与同步诊断）；显示控制台改动仍属于未统一提交的实机图形移植工作区，
完整候选源文件哈希已锁定，不把这些提交当成整个实机镜像的独立干净构建来源。

交付仅更新测试U盘EFI及manifest，桌面分区不写入、不修复，保留本轮真实数据。
实际安装与只读重挂校验以 `usb-readback.json` 和 `physical-root-readback.json` 为准。
旧EFI/manifest保存为 `EFI/A20OS/BOOT17.EFI`、`BOOT17.MANIFEST`，独立shell保留。
下一轮使用同日 `boot-18/collect-logs.py` 收集实机分阶段耗时；当前已有管理员操作授权，
无需用户再次手动复制日志。继续不排查键盘灯，不重启Debian宿主，不改内置SSD分区。

## 第 16 轮实机与第 17 轮 OpenRC 交付（2026-11-13）

第16轮重启“资源忙”的日志已读：1911条连续记录，20次准入因1–2个文件操作返回EBUSY，
没有本轮JBD2 poison。另有明确记录 `busybox -> PID1 i2c-hid, SIGTERM`，证明内核把PID1分给了
早期触控板线程。现在为首个用户态init保留PID1，引导线程退出；桌面使用未修改的Alpine
OpenRC init 0.63及supervise-daemon，平台配置不再实现PID1循环。键盘灯按用户决定停止排查。

内核补齐x86 mknod转换、ramfs/tmpfs/ext4 FIFO、procfs初始环境真实用户页读取，修正kill(-1)
逐线程重复广播，补上FIFO创建的路径权限边界与普通open的类型位屏蔽。旧原型的FIFO回收poison、
环境空占位及一次IBus退出SIGSEGV均留有失败日志，没有放宽验收以通过。广播修复有负对照，
不把它表述为已证明那次用户态崩溃的唯一根因。

最终同一镜像通过16 CPU/8 GiB三组完整桌面与原版服务监督测试：USB普通reboot约9.37 s、
USB图形S5约9.43 s、NVMe图形重启约9.32 s；每组退出后fsck=0、clean、RECOVER=0、last_orphan=0。
这些仍是QEMU结果，新版实机电源动作待第17轮。原版六个OpenRC程序与签名包payload一致，fastfetch保留。

已完成实际USB 7417块/约28.97 MiB更新，写前完整备份核对、写后清缓存完整4 GiB回读、只读fsck均通过。
EFI `530b416363faccb7…` 已安装并在只读重挂后核验；root `a8006c080489d2eb…`。
保留BOOT16及整ESP备份、原shell与其他26个文件，计数00000010未改，三个分区均已卸载。
旧桌面EFI必须与旧root配对，独立shell仍可使用。

详细证据与重建方法：`build/evidence/lenovo-83eg-2026-11-13/boot-16-analysis/README.md`。
内核提交8a9569092，OpenRC配置b6f07c72b；原暂存submodule记录保留。实际构建源由candidate-source.json固定，
未把此前所有驱动移植或文档迁移一并提交。下一轮采集脚本在同日boot-17目录。


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
修复脚本验证完整分区、补丁、可恢复重复执行与意外改动拒绝。用户已完成4块/16 KiB实际修复，
整个4 GiB回读93e9b43a83ef…，只读fsck=0。默认EFI已更新到上述dfe70d20…，旧版保留
EFI/A20OS/BOOT15.EFI与BOOT15.MANIFEST；卸载/只读重挂完整核对EFI、manifest、回退副本、
计数0000000f及23个原文件，三个USB分区均已卸载。第16轮实机重启、RSS界面和灯光待验收。

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

## 第 13 次启动后的当前状态（2026-11-12）

用户已确认 WHU-WLAN 可用，桌面和触控板可以继续测试。本轮内置 ESP 保存 1385 条
连续有效内核记录，启动计数 13；Wi-Fi 关联、授权、DHCP 和 GUI 成功状态均有记录。
新反馈为 CapsLock/NumLock 灯不亮、Firefox 冷启动慢、关机/重启无反应、MAC 非硬件地址。

已定位关机在 VFS admission 阶段返回 EBUSY，尚未进入 ACPI；阻塞字符设备/匿名管道
不应持有 mount transaction。电源服务重试失败也不应退出。补齐 ITE HID LED 输出报告，
将 efuse 硬件 MAC 在 carrier 开启前发布到实际 BSD 协议栈，重启优先使用 FADT reset。
用户提供的 FACP 指定 I/O `0xB0`、值 `0xFB`；HPET 为 MMIO `0xFED00000`、32 位。

日志时间倒退及固定 CPU 的宿主探测又揭示 TSC 跨核 epoch 不一致：invariant rate
不能充当共同 epoch 证明。SMP 优先共享 HPET，停用原始 TSC vDSO，并实现 32 位回绕维护。
HPET32 原生门控捕获通用锁诊断递归调用计时器的启动 double fault，已改用不依赖时钟的原始锁。
完整 Firefox 门控还捕获 ext4 deferred orphan EIO；修复并发 cold lookup 返回同 inode
多个 vnode 的竞态。原生最终结果和实际安装状态统一记录于
`build/evidence/lenovo-83eg-2026-11-11/boot-13/README.md`，失败记录保留，不作为通过依据。

最终 EFI `140a80c0b7db077fbd809f5819cf7a80a99bfafac4f7b708e7f979331df40847`
已通过4 CPU/8 GiB、HPET32 多次真实回绕的两项完整原生门控：USB 桌面/Firefox/LED/ACPI
重启，NVMe 桌面/Firefox/LED/图形 S5。两轮虚拟根分区只读 fsck 无一致性错误。
已更新测试 U 盘 ESP，卸载后只读重挂校验新 EFI、manifest、20个原文件和 BOOT13 回退副本，
启动计数仍为13；三个分区全部卸载，宿主未改写桌面 ext4 或 SSD。
锁定灯的物理效果、永久 MAC 下 DHCP/门户、真实 FADT reset/S5仍待第14轮实机验收。

核心提交：`f55304eda`（共享HPET/32位回绕）、`4693e7b91`（关机门控/设备引用）、
`4d5bc15ed`（ext4唯一vnode）。原暂存条目保留。版本0.19与v0.19 tag一致。

宿主相同 Firefox 文件的 O_DIRECT 测量：USB 顺序约 21.1 MiB/s、随机约 2.745 MiB/s；
SSD 副本约 1940/48.85 MiB/s。该结果支持后续 SSD 部署，但不等于 A20OS 驱动性能。
两块内置盘无现成 4 GiB 空闲区，本轮不缩分区；先验证 NVMe 桌面完整负载。
Firefox 仍为软件渲染，不能只凭介质解释全部卡顿。

## 第 12 次启动后的历史状态（2026-11-12）

实机进入桌面，WHU-WLAN 扫描、5220 MHz 开放网络关联和 DHCP 已成功。
随后 GUI 状态检查失败，重复连接触发已关联 -16；网页认证和外网仍未验收。
本轮触控板持续有真实 motion，一次 I2C 超时自行恢复；未见上一轮 USB 根盘
失效与文件页填充失败链。用户选择 Rime 后顶栏和 dock 消失，旧会话日志为空，
不能仅凭旧日志认定是哪一个进程先退出。

继续原生回归确认并修复：accept 错误继承 listener 非阻塞模式、CLOEXEC 丢失、
匿名 socket 缺少 fstat（导致标准 PyGObject/ibus-setup EBADF），以及相同 Wi-Fi
连接错误地停止 DHCP 后重入关联。增加会话 stderr 到内置 ESP 的镜像与致命信号记录。
Wayland IBus 布局通过同源 vendor override 提供默认值，由 compositor 管理
Rime 的 default layout；保留用户偏好文件，不在启动中写入 gsettings。

长时 QEMU 又捕获 memfd dirty cache 与 vnode 的引用循环：仍有约 37.5 GiB
空闲 RAM 时，遗留的共享缓冲区占满 1 GiB cache，导致文件分配和代码页失败。
同时修复最终 open file description 的 cache 释放，以及页表名称槽位已满时
预留引用却未登记的泄漏；关闭 fd 后仍存活的映射、dup 与
SCM_RIGHTS 所有权继续保留。原失败现场、精确符号和磁盘 overlay 已归档。
继续冷启动设置服务回归，最终确认内核把 D-Bus 对启动辅助进程的正常 SIGKILL
错误扩散给其子进程，误杀设置服务、通知及输入法 portal。已修正 Rust 重托管策略与
C adapter，保留显式 PDEATHSIG 和线程组退出语义；新增信号发起方/强制退出因果日志。

证据、负对照和最终门控结果见
`build/evidence/lenovo-83eg-2026-11-11/boot-12/README.md`。
最终 EFI `ee5d22aa17f3e780a8438104ca0eee219d771e87c9f210d2902523fe935b003c`
已写入测试 U 盘。卸载后重新只读挂载，完整校验 EFI、manifest、18 个原文件与
`EFI/A20OS/BOOT12.EFI` / `BOOT12.MANIFEST` 回退副本，启动计数仍为 12。
宿主未写入桌面 ext4 分区；两个 FAT 分区在交付前卸载。

同一最终 EFI 原生 QEMU 验证：16 CPU、约 40 GiB 内存、25931 次并发 USB 读取、
3 GiB memfd churn、24 组 accept flags、4 组孤儿进程语义、5 个冷启动 dconf profile、
标准 ibus-setup、Rime 实际保存“你好”、Firefox 模拟网页认证及认证后的联网复查。
labwc、xfdesktop、xfce4-panel 在输入法和网络操作后保持原进程。
完整串口、截图、镜像 SHA 与结构化结果绑定于 `qemu-native-matrix.json`。
首次 Wi-Fi fixture 下载因关闭输入法窗口后的焦点竞态丢失首字母，原门控 timeout
完整保留；在同一未更改的虚拟机只重跑 Wi-Fi 阶段通过，未将原结果改成 PASS。
模拟认证验证浏览器和 GUI 通路，WHU-WLAN 实际网页认证与新 EFI 的触控板仍待实机复测。

核心修复已按当前时间提交：`c7ddab6bd`（socket）、`c4a91f46c`（共享缓存/页表引用）、
`b896df173`（父进程退出语义）。其他平台集成、日志和文档改动仍在工作区，
原有文档迁移与两项暂存内容保持不变。

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
`boot-6/synaptics-report-replay.log`。第 7 次启动随后完成了实机桌面验收。

现有测试 U 盘的 ESP 和日志分区保留。部署脚本先保存原 GPT，迁移旧镜像
留下的备份 GPT，再在剩余空间创建桌面分区。读回校验 ext4 后才切换默认
EFI，同时在 ESP 的 `EFI/A20OS/SHELLX64.EFI` 保存第 6 次验收的 shell。
### 第 7 次实机桌面验收

用户确认 XFCE 可用，触控板无响应，系统约显示 6 GiB 和 4 个逻辑处理器。
内置 ESP 的 `CURRENT` 为 `bootcount=7`、`state=ready`，保留了序号
0–633 的 634 条记录。4.744 s 挂载 U 盘桌面分区，31.964 s 首次提交
1920×1080 画面。内核、UEFI 和桌面会话原始日志归档在
`build/evidence/lenovo-83eg-2026-11-10/boot-7/`。

- CPU 在 4.774 s 报告 `4/4 configured CPUs online`，正好达到桌面实例
  的编译上限。Debian 基线为 Ryzen 7 7840H 的 8 核 16 线程。下一版
  桌面实例将上限设为 16，并让各 CPU 共用按 GS 定位本地数据的
  syscall 入口，仍由 MADT 决定启动哪些 CPU。
- 内存日志中 4 GiB 以上只有 `0x100000000..0x200000000`，旧内核把
  直接映射限制在物理地址 8 GiB 以内，并丢弃高地址不足 1 GiB 的尾部。
  Debian 基线的 `MemTotal` 为 40,337,540 KiB，最高 RAM 地址为
  `0xa3e2fffff`。下一版映射 64 GiB 物理地址范围内的固件可用页，
  以 4 KiB 映射保留区边界，避免吞掉相邻 RAM。物理页元数据在内核
  所在连续区放不下时，改放到另一段足够大的可用区。
- 触控板在 2.133 s 完成 HID 枚举，注册为 `input/event1`，10.093 s
  的首个运行时输入读取返回 `-ETIMEDOUT`。当时的首个报告和指针事件
  日志来自自检。之后 eudev 的触控板探测和其他输入设备 worker 也超时。
  下一版从 `_CRS` 解析 `AMDI0030` 的低有效 GPIO9，后台线程只在线路
  有效时发起输入读取。evdev 只访问事件队列，DesignWare 等到 STOP 或
  abort 才结束传输。日志分别记录 GPIO 首次有效、复位应答、触控模式
  设置、首个真实报告及首个入队事件。
  [Linux AMD GPIO 读取](https://github.com/torvalds/linux/blob/v6.12/drivers/pinctrl/pinctrl-amd.c)
  · [Linux HID-over-I²C 输入路径](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/i2c-hid/i2c-hid-core.c)

下一次实机启动后，用 `Super+Enter` 打开终端，运行
`grep -c '^processor' /proc/cpuinfo` 和 `grep '^MemTotal:' /proc/meminfo`，
然后用内置触控板移动指针并按下点击。按启动号归档内置 ESP 日志。

用于第 8 次启动的 EFI 已写入实机测试 U 盘并重新挂载读回校验，
SHA-256 为 `54bcd94f2d5dc054a4aec543403d66cdec83b4d24e91c1ac3ac750a83d2cbc04`。
第 7 次的桌面 EFI 保留在 `EFI/A20OS/DESKTOP7.EFI`。同一 EFI 在复刻
实机三分区布局的 QEMU 中，从 USB hub 下游挂载 XFCE。40 GiB、16 vCPU
的来宾进入桌面后，用 USB 键盘打开终端，读到 16 条 `processor` 记录和
`MemTotal: 41935276 kB`。I²C 主机模型和 HID 回放通过，真实触控板
的移动与点击待第 8 次启动验证。记录见
`boot-7/qemu-physical-layout-40g-16cpu.log` 与 `boot-7/usb-readback-sha256.txt`。

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

## RTL8852BE 无线候选实现（2026-11-11）

用户新增需求：内置无线网卡连接 Wi-Fi 上网。Debian 本机的 PCI 证据为
`03:00.0 10ec:b852 / 17aa:4853`，驱动 `rtw89_8852be`。
新实例启用 `wifi_rtw89=true`，从固定 submodule 构建独立 `rtw89d`，
通过内核的 PCI/DMA/Ethernet transport 接入 BSD 网络栈。
固件与工具随 EFI 内嵌，启动时复制到 `/a20` 的 tmpfs；原桌面分区可继续使用。
使用方法、精确来源、日志及实机验收标准见
[RTL8852BE 无线网卡](../drivers/classes/rtl8852be-wifi.md)。

集成检查发现两处启动问题并已修复：A20OS 暂无 bind mount ABI；内核原先
直接 chroot 到 distro init，绕过了内置 init 的服务准备。Wi-Fi 实例现在先进入
内置 init，准备工具和服务后再执行原有 `/sbin/init`。无 Wi-Fi 的实例维持原路径。

主机运行 215 项测试：214 项通过、1 项跳过，包含平台回调取消、重定时、执行中释放与锁
所有权测试。完整桌面模拟已执行内嵌无线协议自检，通过 QEMU E1000 实际获取
DHCP 租约并读回默认路由、DNS。记录保存在
`build/evidence/lenovo-83eg-2026-11-11/wifi/`。
与测试 U 盘相同三分区布局的最终 EFI（SHA-256
`61723c6c8ea78710319ebb01ae3b841bae3f46175c4cf9691b9317e50b6c431a`）
还通过了 DNS + HTTPS 请求。已替换测试 U 盘的默认 EFI 和 manifest；
前一候选版保留为 `EFI/A20OS/DESKTOP8.EFI` / `DESKTOP8.MANIFEST`，
启动计数仍为 7。原有 DESKTOP7、shell、日志及桌面分区保留。
RTL8852BE 的固件启动、实际热点扫描、密码认证、无线收发和互联网访问仍待实机验收。

### GUI 与 WHU-WLAN 网页认证

用户要求改用 GUI，目标热点为 `WHU-WLAN`。Debian 的当前连接与缓存扫描确认该
SSID 无无线层密码，在本机可见 2.4 GHz 和 5 GHz BSS（当前连接为频道 44）。
武汉大学官方说明确认 Web 认证；未使用任何真实校园网账号进行自动登录。

Wi-Fi 工具现提供 GTK 3 窗口、桌面快捷方式及应用菜单入口，支持刷新、选择、连接、
断开、认证页打开和网络状态复检。开放热点发空无线密码，获取 DHCP 地址后检测
传统 HTTP 门户或可用的 DHCP option 114 / CAPPORT API，认证页面交给正常 Firefox。
联网结果须经 HTTP 和可信 HTTPS 检查，不以 DHCP 租约作为完成网页认证的证据。

QEMU 完整打包镜像已通过 GUI 与真实 Firefox 的门户测试：关联由测试 socket 模拟，
HTTP 302、JavaScript 表单、POST、Cookie 和重定向在 A20OS 中实际执行，登录后
GUI 再次检测为在线。9 项主机测试通过，包含无密码协议、SSID 原始字节、重定向、
CAPPORT 解析与证书错误处理。记录位于
`build/evidence/lenovo-83eg-2026-11-11/wifi-gui/`。
此测试认证页为本地 fixture，不等同于 WHU-WLAN 的实机验收。

GUI 最终 EFI SHA-256 为
`2e191bb400d64f43ad2d0476b57798a1b505a017af0e75b01fcf45df55ead1e9`。
它已替换测试 U 盘的默认 EFI 与 manifest，并经卸载、只读重挂验证。
前一命令行版保留为 `EFI/A20OS/WIFI-CLI.EFI` / `WIFI-CLI.MANIFEST`。
最终模拟记录为 `wifi-gui/qemu-final-fixed.log`：包含实际生产入口启动、桌面快捷方式、
密码框取消、诊断弹窗和完整 Firefox 门户流程，无 Python 异常。
早期 `qemu-final.log` 发现 `Gtk.Dialog.run()` 触发 Python signal wakeup fd 的
`EBADF`；已统一改用 `GLib.MainContext` 循环，验证后才写入测试 U 盘。

## 第 8 次实机：触控板仍不可用

用户确认触控板无响应。`CURRENT` 指向 `7064814b2698f8b3212f52d9429e2f81`，
bootcount=8，state=ready，560 条记录；归档在
`build/evidence/lenovo-83eg-2026-11-11/touchpad/A20LOG/`。

- GPIO9 可用，2.092 s 收到复位完成；67.952 s 收到真实 report 3（32 字节），
  并送入指针事件队列。采样期间也记录了 libinput-fuzz-extract 探测超时。
- 实际 665 字节描述符包含 InputMode 4、LatencyMode 13、Surface/ButtonSwitch 6。
  初始化现补齐 3/0/3，并通过 GET_REPORT 读回，分别记录结果。
- evdev 增加 EVIOCGMTSLOTS 及每设备独立槽状态；空闲 tracking id 为 -1，
  追踪范围从 0..0 修正为 0..65535。真实 host libevdev 快照正/负对照已验证；
  host libinput 会容忍缺失槽查询并修正部分元数据，因此这些修复不能单独证明
  本机故障已经消失，下一步仍需实机验证。
- 增加自动用户态探测：真实 evdev 轴范围、槽查询、libevdev 和 libinput 接受结果
  写入 touchpad.log 与 A20LOG；内核每 30 秒汇总读取、触摸、移动及错误计数。
  探测不会读取或记录键盘事件、触控坐标。

发行版 90-libinput-fuzz-override.rules 会在设备登记过程中启动日志中的超时 helper。
这条原生驱动的 fuzz 全为零，下一候选仅对 `I2C HID touchpad` 跳过 fuzz 提取和
归零 helper，其他设备保留原路径，避免无效转换阻塞设备登记。
覆盖文件经 udevadm 校验，且已在同 U 盘布局 QEMU 中读回。

修复 EFI SHA-256：`f254b2090445f8e1d00ce5352896b6d5de2ba17c0a1b0bffe42a8b38cfc6589d`。
已用于更新测试 U 盘，第 8 次 GUI EFI 备份为 `EFI/A20OS/GUI8.EFI` 与相应 manifest。
启动计数保留为 8。下一次实机需验证移动与点击，并检查自动 `[TOUCHPAD]` 记录，
不能以 QEMU 启动或合成库测试替代本机触控板验收。

## 第 9 次实机：触控板基本可用，输入中断与无线地址错误

用户确认 `f254b209...` 版触控板已可用，但在网络配置窗口点击按钮后失效。
boot `be6e22834c216b630e78e59bcf83e467`（607 条）证明真实 libevdev 初始化为 0，
libinput accepted=True / pointer=True；累计 2152 份有效报告、1362 个移动帧。
80 秒发生一次 I2C 超时，随后仍有报告，不能直接断言网络按钮使总线永久停止。
详见 `build/evidence/lenovo-83eg-2026-11-11/boot-9/README.md`。

发现并修复无线 transport 的确定性缺陷：本内核 usercopy 成功返回字节数，旧 broker
却把非零成功值解释为 EFAULT，导致 INFO 初始化即退出。PCI 配置、DMA、link 的
判断一并修正。网卡 BAR2 的实机值仍为 `a1a00000`，无需盲目改物理地址。

输入侧增加整帧发布、源队列溢出的 SYN_DROPPED 与完整状态重建、超时后的触点/按键
释放及 reset/报告模式恢复；DesignWare 失败时执行 ABORT 后再 disable。
模拟输入自检覆盖积压溢出、丢失 release、读超时、reset 和下一次正常触摸。
自动诊断延长至 180 秒，记录 libinput 移动、按下、松开计数；网络按钮与无线初始化
阶段直接进入 ESP 日志。下一轮需验收连续操作后的触控板，以及真实热点扫描和关联。

最终 EFI 为 `9bb05563975989e55d58dca8e49df244115314f4e0ce44cfb961f8878347f0fb`。
15 项主机测试与完整 QEMU 桌面/输入恢复/模拟网页认证门控通过；测试 U 盘已更新，
旧镜像备份为 `EFI/A20OS/TOUCH9.EFI`。卸载后重新挂载读回一致，12 个既有日志与
备份文件未改变，启动计数保留为 9。连续输入与真实无线联网仍待第 10 次实机验收。

## 第 10 次实机：JBD2 格式变化导致桌面挂载失败

boot `5410bdcb894e909f9061167c6bff61b2` 的 524 条日志确认：桌面根在 4.893 秒
因 JBD2 `-95` 被拒绝，系统进入 RAM shell。I2C-HID 的周期记录没有读取错误、
没有复位重试，并实际收到了触摸报告；屏幕出现这些记录不能作为它阻塞桌面的证据。

测试分区的 journal checksum v3 位已被清除，只剩 64bit；当前 filesystem VALID、
journal start=0、errno=0、无 recover flag。Linux 挂载可能调整 journal 校验特性；
普通 `ro` 也不能保证不写 ext4。此前普通 ro 检查是不充分的取证措施，今后读取该
测试分区须使用原始只读 FD 或 `ro,noload`；未经恢复的内容不能当成一致性验收。

新内核仅对干净、空、UUID/几何匹配、无错误且只含已知基础 feature 的无校验日志，
同步恢复 checksum v3 后再启用 writer；脏日志、v1/v2 与未知格式仍拒绝升级。
周期触控板状态改为仅保存到日志，错误仍显示。17 项主机测试通过。

已完整备份实际桌面分区，在副本上修复字体缓存临时文件引用计数、空闲计数及目录
索引，e2fsck 强制复查通过。使用实际 GPT/ESP 与修复副本进行对照：旧内核复现相同
JBD2 -95，新内核升级后进入桌面、16 CPU/40 GiB 和 USB 输入通过，再以同一写入盘
验证再次启动。证据位于 `build/evidence/lenovo-83eg-2026-11-11/boot-10/`。

用户已完成管理员分区修复，整个 4 GiB 回读与 e2fsck 通过的修复副本 SHA-256 一致。
默认 EFI 已切换为 `f598641a92936b439afbbfec3c913cc70f243b39dba07ccee70122168ae84e55`，
重新挂载读回一致，14 个既有日志/备份文件未变；旧内核备份为 `EFI/A20OS/TOUCH10.EFI`。
启动计数仍为十六进制 `0000000a`（10），下一轮第 11 次实机验收待进行。

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
