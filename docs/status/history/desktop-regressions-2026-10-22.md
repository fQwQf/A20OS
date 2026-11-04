# XFCE、Firefox 与 Minecraft 回归修复（2026-10-22）

本轮根据 `[HFF] cache_get NULL`、XFCE 组件崩溃、Firefox 子进程退出和 Minecraft 自动选择 RISC-V 资源的问题修复。GUI 实测平台为 x86_64/QEMU KVM， 4 CPU、4 GiB；Firefox 使用普通 virtio GPU，Minecraft 使用 virgl/AMD Radeon 780M。AArch64、RISC-V 本轮完成交叉构建，未重复验收对应架构的 GUI。

## 根因与改动

- 页缓存每次回收只扫描相同的 128 个尾部条目，固定重试次数无法越过被固定的页面；即使其他位置有可回收页，也可能达到容量上限并向共享映射返回 NULL。 Rust 扫描器现在推进扫描位置，调用方允许完整的两轮 second-chance 扫描，并恢复缓存命中时的 accessed 标记。全池不可回收时仍有界返回。
- Linux clone 的 Rust 事务没有把 `CLONE_SIGHAND` 交给信号初始化逻辑，线程各自复制处理表。Firefox 在工作线程创建后安装的 SIGSYS 处理器无法被这些线程看到，沙箱的 PR_GET_SECCOMP 回调直接触发默认终止。现在线程按标志共享处理表，普通 fork 保持复制；exec 在不可逆提交前分离处理表，避免重置他人的处理器。保留 Firefox 沙箱。
- 系统调用重启统一回退 4 字节，破坏 x86 两字节 syscall/int 指令的重放。重启现在使用架构提供的指令长度。
- ext4 批量写回在第一个空洞之后继续分配已有映射的块，extent 增长还按追加处理随机写入。现改为只分配连续空洞、有序插入/合并 extent，并正确拆分叶子。原镜像中 Firefox startupCache 的乱序 extent 可以由旧代码对照测试复现。
- fast symlink 的内联目标被错误地当作块映射处理，导致 orphan 恢复失败。修正内联链接回收；full profile 默认采用已有的 Rust extent 事务，以支持正常桌面文件产生的 depth-1 树。修正 Rust 挂载状态机的脏卷恢复和刷盘重试。最终关机在 VFS/MM 写入门关闭后回收残留 orphan，再持久化 CLEAN。
- Minecraft 启动器此前取资源目录遍历的最后一项，混合架构时会选中 RISC-V。现在按实际 `uname -m` 和 native ELF machine 选择，显式 MC_HOME 也做检查；版本从 version.json 读取。镜像自动打包使用同一选择器，架构专用 make 入口让子 make 按目标架构计算默认值，保留显式 GUI_MEDIA。XFCE 显式依赖 Python。

## 验证

本地证据目录：`build/desktop-repair/`（不提交生成镜像和日志）。

| 检查 | 结果与证据 |
| --- | --- |
| 页缓存 Rust 单元测试 | 18 通过，`page-cache/rust-test.log` |
| 页缓存 C/Rust 差分 | 200,000 次操作通过，四个 Rust 目标编译通过；`page-cache-host.log` |
| clone Rust 事务 | 47 通过，`clone-host.log` |
| ext4 事务、挂载状态 | 52 + 7 测试通过；`ext4-host-final/rust-unit-tests.log`、`mount-state-final.log` |
| ext4 实盘 host fixture | 稀疏覆盖、400 个乱序 extent、叶子拆分、内联链接、崩溃恢复、持引用关机、截断与故障重试；C/Rust 两选择器及独立 e2fsck 通过；`ext4-host-final.log` |
| Minecraft 架构选择 | 混合架构、自定义目录、错误显式路径、缺失 native、manifest 版本和数字排序；4 测试通过 |
| 内核综合回归 | `smoke-vm-fault-x86_64` 在 KVM 下完整通过；`native-suite/vm-fault-x86_64.log`、`vm-smoke-kvm.log` |
| 内存锁模型 | 16 条断言通过，`mm-lock-gate.log` |
| Firefox | 配置读写、HTTP/HTTPS DOM、Bing 搜索、刷新、标签页、5 分钟持续 DOM 检查通过；`sighand-web.log`、`firefox-pass.png` |
| Minecraft | 自动选中 x86_64；demo 世界生成、移动、跳跃、背包、保存、正常退出和冷启动重载通过；`minecraft-play.log`、`minecraft-reload.log` |
| 实际桌面关机镜像 | 重载后再次保存、正常退出、关机；独立 `e2fsck -fn` 全部通过，`minecraft-final.e2fsck` |

Minecraft 实测约 12–17 FPS。第一次保存的 level.dat SHA256 为`426bd5897bee91d347d7cc0741cc2264051449b5ab345306f523e12669c84606`，关机后宿主离线读取及冷启动客体读取均一致。NBT 保存位置`[-41.5, 70.0, 65.69999998807907]` 与冷启动的实际玩家登录位置一致。这不代表 Microsoft 账号登录、Realms 或多人服务已验收。

最终实测内核 `final.elf` SHA256：`c4b203d38fcd7417491fb7f55387ef09f6b463f2c975c3828c9f17f370269f65`。画面证据：`firefox-pass.png`、`minecraft-world-before.png`、`minecraft-moved.png`、`minecraft-inventory.png`、`minecraft-reload-coordinates.png`。

负对照：旧内核运行新增的 syscall_restart_test 和更新后的 seccomp_context_test 均失败（`negative-native.log`）；旧 ext4 代码运行新增桌面 fixture 失败，e2fsck 报告乱序 extent、块计数和位图问题（`negative/`）。旧页缓存扫描器无法通过新增的被固定尾页测试（`page-cache-negative.log`）。

原综合脚本的 TCG/120 秒运行在 gs_base_test 处超时；同一测试集在 KVM 下完整通过。架构边界总门禁仍有一个 HEAD 已存在的问题：`kernel/main.c` 的 CONFIG_PCI_PIRQ_TEST/CONFIG_X86_64 条件。本轮没有新增该类架构分支。

## 使用与已有镜像

`tools/a20 run xfce-x86_64` 会重新构建内核和 world，安装修正后的启动器。 Minecraft 的 3D 实测配置对应 `minecraft-x86_64` 实例；普通 XFCE 的 2D GPU 配置不因此获得 virgl 能力。多架构资源下载及启动方式见[Minecraft 文档](../../distro/minecraft.md)。

原 `build/images/xfce-x86_64.img` 已有 extent/orphan 损坏，本轮保留原文件，仅修复和操作 `build/desktop-repair/world.img` 测试副本。代码修复不会自动找回旧损坏 extent 的内容；测试使用独立 Firefox profile 与独立游戏存档，避免把旧文件损坏混同于新内核行为。

后续于 2026-10-23 按用户要求清理旧镜像；删除前保留了用户目录备份。最新实测镜像与证据保留位置见[软件、apk 验证记录的清理说明](desktop-apps-apk-2026-10-23.md#提交与产物清理2026-10-23)。
