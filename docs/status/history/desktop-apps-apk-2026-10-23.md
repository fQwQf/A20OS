# 桌面常用软件、中文输入与 guest apk 验证（2026-10-23）

本次把已通过基本功能验证、无需在线账号的应用加入 `packages/world/xfce.world`。
保留 Firefox，不重复安装 Chromium；不加入 Telegram、Steam。Steam 尚无成功
运行证据，不能据此声称兼容。

## 实测范围

运行环境：x86_64 QEMU/KVM、4 CPU、4 GiB RAM、virtio GPU/网络/块设备，
A20OS 内核 + Alpine v3.23 musl 用户态、labwc/Xwayland/XFCE。验证使用私有
4 GiB ext4 镜像，未覆盖原有 `build/images/xfce-x86_64.img`。

其他架构仅验证了最终 world 的 APK 依赖求解，未进行 GUI 运行验证。

| 应用 | 实测结果 | 默认 world |
| --- | --- | --- |
| Geany 2.0 | GUI 键盘编辑、保存、读取；Rime 输入 `nihao` 并保存为 `你好` | 加入 |
| Evince 48.4 | GUI 打开 Writer 导出的中文 PDF，文字正常显示 | 加入 |
| LibreOffice 25.8.1.1 Writer | 中文 TXT → ODT → DOCX → PDF；GUI 编辑后保存，检查 ODT 内容 | x86_64、AArch64 加入 |
| LibreOffice Calc | CSV → XLSX；检查中文单元格、`SUM(B2:B3)` 与缓存结果 `42`，GUI 显示正确 | x86_64、AArch64 加入 |
| KeePassXC 2.7.11 | 使用专用虚拟密码创建 KDBX、添加条目、解密导出校验；GUI 解锁并显示条目 | 加入 |
| GIMP 3.0.4 | PNG 载入、缩放为 256×192、保存并重开 XCF、导出 PNG；GUI 打开图片 | 加入 |
| IBus/Rime | 默认会话启动、简体拼音方案、Geany 实际输入与保存中文 | 补齐模块、词库与启动配置 |
| VLC 3.0.21 | 拒绝以当前 root 桌面用户启动 | 不加入；保留现有 Parole/mpv |

Alpine v3.23 的 RISC-V、LoongArch 仓库没有这些 LibreOffice 包，因此 world
使用 `[x86_64,aarch64]` 前缀选择安装。四种桌面架构的最终清单均通过依赖求解。

IBus 使用 labwc 的 input-method-v2 与 XIM 桥接；`ibus engine rime` 切换后已
实际输入中文。上游默认热键为 Super+Space，但本次 QEMU 按键复测未可靠切换，
不能把该热键记作已通过。没有全局强制 `QT_IM_MODULE=ibus`，该设置曾
阻塞 KeePassXC GUI。首次使用 Rime 会部署词库。

Alpine 的 xfce4-notifyd 缺少 Wayland 窗口支持，D-Bus 服务现在通过
`GDK_BACKEND=x11` 使用 Xwayland；实测 GetServerInformation 正常返回，消除了
Rime 首次部署时的通知弹窗与超时。

GIMP 在 x86_64 上通过 `/usr/local/bin/gimp` 过滤 babl 的 AVX 扩展。当前内核
没有完整 AVX/XSAVE 用户态上下文支持，而部分扩展在 CPU 能力检查前就执行 AVX。
其余架构和显式提供 `BABL_PATH` 的调用保留上游模块选择。默认关闭未验证的 OpenCL。

## A20OS 内的 apk

清单加入 `apk-tools`、`alpine-keys`；镜像组装还把构建使用的 `*.rsa.pub` 公钥
复制到 `/etc/apk/keys`，不复制私钥。保留 HTTPS 仓库与签名检查。

在真实 guest 内完成：

```sh
apk update
apk add tree
tree --version
apk del tree
apk add tree
```

验证了主仓库和 community 索引、安装/删除、busybox 触发脚本、再次安装及运行。
删除独立 tree 包后，busybox 触发脚本会恢复自己的 tree 符号链接，这属于预期行为；
应检查 `apk info -e tree`，不能用路径是否存在判断独立包是否仍安装。
使用空密钥目录读取原始签名索引会被拒绝，实测出现 `UNTRUSTED signature`。
没有使用 `--allow-untrusted`。

本次没有验证完整 `apk upgrade`，也没有保证任意 Alpine 包都能运行。

## 为实际运行修复的问题

- x86_64 的 `renameat` 错误映射到 `renameat2`，把无关寄存器当成 flags，导致
  APK 索引提交和安装改名失败。修正系统调用映射并加入参数回归。
- chroot 内执行相对路径脚本时，exec 把物理 `/extra/...` 路径传给解释器，导致
  APK 触发脚本打不开自身。保留调用者路径，物理路径仅用于打开的文件元数据。
- SysV 共享内存执行 `IPC_RMID` 后，只要仍有附件就应允许再次附加。修正 Rust
  状态机与 C 后备路径，GIMP 插件的默认共享内存模式通过跨进程验证。
- 大 TCP pbuf 链拆分后可能超出整个 BH 环容量，永远无法重新接收，APK 下载停顿。
  改为整链单事件与溢出槽持有，并在消费后释放溢出 pbuf。
- 全局 sync/压力写回以前没有获得目标 vnode 的写回锁，可能与 fsync 并发发布
  同一 inode 的 extent 快照。全局批次也获得目标 vnode 锁。
- 新脏页在发布文件长度前已经可见；写回可能按旧 EOF 跳过它，却将其标为干净。
  现在每页在页锁内先发布 EOF，再发布脏状态，部分成功也保留已写入长度。
- ext4 读/写回持有的旧长度可能覆盖并发写入增长后的长度。普通发布改为原子增长，
  缩小由 truncate 路径负责。测试注入写回期间增长，确保旧快照不能缩小 vnode。
- 向含有已映射块的稀疏窗口批量分配可能重叠 extent；分配限制在第一个已映射块前。
- mkfs 导入的 HTree 索引目录被线性目录写入破坏。修改前先转换为带校验尾的线性
  目录并清除索引标志；保留原叶块内容，拒绝已经损坏的根结构。
- 首次分配延迟初始化块组时，未完整初始化位图、保留元数据块或清除 UNINIT 标志。
  现在按组元数据范围构造位图并检查空闲计数，更新标志与校验和。

这些修复不能恢复旧镜像中已经丢失的字节。已有损坏应先备份，并从已知正确来源
重建文件或镜像；本次用全新、初始 fsck 干净的镜像完成持久化验证。

## 回归与证据

证据目录：`build/desktop-apps-2026-10-23/`（构建产物，不入版本控制）。

- `profile-tests.log`：world 架构选择、公钥复制与 Minecraft 选择器，9 项通过。
- `architecture-solves.json`：x86_64、AArch64、RISC-V、LoongArch 依赖求解均返回 0。
- `page-cache-eof-tests.log`：20 项 Rust 测试、200000 次 C/Rust 差分，四个 Rust
  裸机目标编译通过。
- `ext4-eof-host.log`、`ext4-eof-host/`：真实 ext4 C/Rust 两种选择器，截断、
  注入 EIO 后重试、稀疏写入、孤儿符号链接、EOF 并发增长、HTree 修改、延迟块组
  分配；独立 e2fsck 均通过。
- `runtime-eof-final.log`：guest 脚本/共享内存 ABI、GIMP、KeePassXC 与通知服务。
- `chinese-fresh-final.log`、`chinese-final.png`：全新默认配置下实际保存 `你好`。
- `office-content-proof.log`、`writer-gui-save-proof.log`、`writer.png`、`calc.png`、
  `evince.png`、`keepass-unlocked.png`、`gimp-image.png`：应用内容与 GUI 证据。
- `apk-eof-positive.log`、`apk-trust-negative.log`：APK 安装生命周期与签名拒绝测试。
- `disk-after-shutdown.log`：并发 sync/fsync 写入 4×8 MiB 文件，正常桌面关机后，
  使用宿主 debugfs 直接导出磁盘文件；四个文件哈希全部正确。两个 APK 索引也与
  关机前哈希一致。
- `fsck-eof-after-shutdown.log`、`fsck-final-reboot-shutdown.log`：正常关机以及
  重启后再次正常关机的 e2fsck 五阶段检查均干净。
- `writeback-after-reboot.log`、`apk-cache-after-reboot.json`、`desktop-after-reboot.log`：
  同一镜像重启后，四个压力文件与两个索引哈希仍正确；缓存索引搜索与已安装 tree
  执行成功，中文文件保留。

可重复运行的压力程序：`tools/tests/desktop_writeback_probe.py`。
**必须在重启后再次验证**；热缓存正确曾掩盖真实磁盘缺页。

```sh
# A20OS guest 中
python3 desktop_writeback_probe.py write --directory /root/writeback-qa
# 正常关机、重新启动同一镜像后
python3 desktop_writeback_probe.py verify --directory /root/writeback-qa
```

后续重启复测还有两项未解决的桌面稳定性问题：

- Geany 一次在 gdbus 线程触发 musl malloc 检查（`HLT`，进程收到 SIGSEGV）。
  重试可启动，之后 Wayland/X11 各 6 次启动均未复现；尚未确定根因。
- IBus 一次在重启后后台 daemon 退出，热键未生效；重新运行
  `/usr/lib/a20/start-input-method` 后恢复 Rime。全新镜像中中文输入保存已通过，
  仍不能承诺跨重启的输入法会话稳定性。

证据：`serial-eof-reboot-desktop.log`、`late-desktop-progress.log`、
`chinese-shortcut-after-reboot.log`。这些问题没有被记作修复成功。

本次 GUI 仅做上述基本功能检查，未覆盖长期稳定性、完整文档格式兼容性、所有
输入法工具包、硬件 GPU 加速或全部应用插件。

## 提交与产物清理（2026-10-23）

按用户要求，后续已删除旧的损坏 `build/images/xfce-x86_64.img`、失败镜像副本、
重复镜像 fixture、抓包、重复 PPM 和临时软件包镜像站。删除前已将旧 XFCE 镜像
中的配置、用户数据及 Minecraft 目录备份到
`build/desktop-submit-cleanup-2026-10-23/old-xfce-user-files.tar.gz`。

最新实测镜像 `build/desktop-apps-2026-10-23/world.img` 及其 boot.img 保留，
测试日志、PNG 截图和文件哈希记录保留。已删除的 fixture 可用仓库内测试脚本重建。
清理清单见同目录 `cleanup-result.json`；其他任务的镜像与未提交代码未纳入清理。
