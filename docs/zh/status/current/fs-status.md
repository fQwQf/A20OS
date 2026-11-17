# 文件系统当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

各文件系统后端的当前支持范围与限制。2026-11-16 范围更新：新增 ACL 权限/继承、xattr 命名空间与后端持久化属于 [后续功能](../../roadmap/next-horizon.md#后续版本功能强化)；已有实现与验证记录继续保留。

2026-11-17 范围同步：原 N21（ext4 orphan 收尾专项）已按[发布范围 §2.6](../../releases/v1.0.md#26-ext4-专项任务移除)从 1.0 任务中删除。本页保留实际支持范围、历史修复和验证边界，不再维护该专项待办。

<!-- 以下内容从 docs/zh/fs/ntfs.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 路径访问与 Landlock

普通 open、openat2 与 proc fd 重开在创建、截断、后端 open 前检查解析后的目标路径。普通 open 与 openat2 使用同一次遍历返回的 vnode 与 canonical path，避免检查原始符号链接名后访问另一个目标。READ_FILE/WRITE_FILE 子集及继承语义见[安全候选验收](../../security/release-acceptance.md)；该路径策略不提供 inode 锚定的 Linux 完整 Landlock 语义。`smoke-security-release` 覆盖符号链接、相对别名、openat2、 proc fd 重开与创建拒绝，并验证没有截断/创建副作用。

## ISOFS

只读；名字按 ISO 9660 规则转小写并去掉 `;1` 版本后缀。没有 Rock Ridge / Joliet。

测试镜像由 `tools/fs/mkisofs_test.c` 生成，刻意覆盖这几类形状：跨多个 2048 字节逻辑块的根目录（记录不跨块，块内剩余部分补零，扫描需要续到下一块）、200 字节长名（贴近驱动`ISOFS_MAX_NAME` 上限）、零长文件、恰好一个块的文件、跨块文件，以及嵌套目录。`smoke-vfs-stress` 在 guest 内读回全部这些文件（含首块与末块目录项），并检查 EOF 之后读返回 0、缺失名字返回 `ENOENT`、写打开失败。同一张镜像也供用户态 ufsd 的`smoke-native-fs-all` 使用。

文件读取走页缓存；`readpage` 的返回值是页内有效字节数，页缓存会把该值之后的部分清零。

## ext4

> 交付形态的 world 镜像（`tools/pkg.py image-world`）现在默认带内部 JBD2 journal；`mkrootfs.py --journal` 在 `mkfs.ext4` 之后补写 JBD2
> `64bit|csum_v3` 标记，因为 e2fsprogs 不会自动设置 A20OS 驱动要求的这个
> journal 特性。开发用崩溃测试镜像仍走 `tools/img.py` 的无日志路径。


有日志与无日志（`^has_journal`）两种镜像都支持完整的 create/write/unlink/rmdir 回收：

- unlink 把零链接 inode 挂到孤儿链（`s_last_orphan`，`i_dtime` 作 next），崩溃后由挂载时的 recovery 处理；延迟回收按 metadata_lock 保护的 vnode 归属记录判断，链上 next 改动不影响归属；inode 成功释放时立即撤销旧归属，编号复用后旧 vnode 不再误 reap 新对象；
- rmdir 将空目录的两个链接一起降为零并登记到同一磁盘孤儿链；仍被引用的目录在最后释放时走可重试的 checked truncate/reap，服务退出后挂载恢复也能回收它。已删除目录拒绝新建子项；
- `metadata_csum`（superblock/group descriptor/bitmap 的校验和）在写入路径同步，`e2fsck` 不报校验和错误。
- xattr：`user.*` 与两个 POSIX ACL 属性名持久化在 inode 尾部（合计约 90 字节/inode，超出返回`ENOSPC` 且不改动已有属性；没有外部 EA 块）。`trusted.*`/`security.*` 仍只在易失表里。ACL 只存储与读回，不做权限判定。门禁 `smoke-ext4-xattr`：写入 → fsync → 关机 → 宿主 `debugfs ea_list`看到属性且 `e2fsck -fn` 干净 → 重启后读回一致，并验证替换与 `ENOSPC` 路径。

正常关机后宿主 `e2fsck -fn` 干净，并且这条性质有门禁：`smoke-vfs-stress` 在 guest 跑完（含 create/unlink/rmdir）之后会对同一张镜像跑一次 `e2fsck -fn`。

无日志镜像被**直接 kill**（不是正常关机）后，下一次挂载会认出 `s_state` 的脏标记并跑孤儿 recovery 再挂载，不拒绝：孤儿 inode 在 recovery 里被截断并释放，恢复后 `e2fsck -fn` 干净。`smoke-ext4-recovery` 就是这个场景的门禁（open+unlink → kill → 重启同一张镜像 → 检查挂载成功、 inode 已被回收、两次 `e2fsck -fn` 都干净）。仍然拒绝的是 `ERROR_FS`：那表示驱动自己在上一会话里发现过不可信元数据，挂载期 recovery 修不了它。

需要日志级崩溃原子性（提交点前后哪些写在崩溃后存活）时用带日志的 `ext4-journal.img`，由`smoke-ext4-journal` 覆盖（4 个注入点的崩溃 + 恢复后 `e2fsck` 干净）。 2026-11-16 在 RV64 SMP2 上重跑：clean 控制及 post-recover-flag / post-journal / post-commit / post-checkpoint 四个切点全部通过，共 10 次自然退出的 guest 启动；前两点丢弃未提交事务，后两点成功重放，崩溃后和恢复后的 `e2fsck -fn` 均退出 0。门禁逐提示符输入并回答 BusyBox 光标查询，拒绝超时杀进程、异常退出及内核诊断；记录内核/基线/各 postimage SHA-256。证据：`build/evidence/v1-closeout-20261116/ext4-journal-crash-final/report.json` 与同目录 guest/fsck 日志。后续持续 I/O 错误与 revoke 验收见下文。

2026-11-16 的持续设备写错误矩阵进一步发现并修复 checkpoint 顺序缺陷：`SYNC_HELD` 需要保留 hold 位才能选出元数据页，原先提前释放会空刷并过早清日志。现在仅在 home 写入与 flush 全部成功后释放 holds；C 后端像 Rust 控制器一样保留写日志、已提交待 checkpoint、待清日志、待清恢复位各阶段，失败后禁止修改原事务。

`tools/fs/ext4_journal_io_gate.py` 在真实 QEMU blkdebug 块设备上持续返回 EIO，覆盖 ordered flush、descriptor、payload、commit 与 checkpoint bitmap。前三类超过 VirtIO 三次重试后保留未提交事务；批量 checkpoint 返回硬错误后保留已提交日志。每例都先观察到内核明确拒绝脏盘关机，再切断模拟电源；移除故障后的同盘冷启动正常退出。提交前文件正确丢弃；checkpoint 失败后的 1 个事务/14 个块成功重放，恢复后各例 `e2fsck -fn` 均退出 0。断电前 checkpoint 的 e2fsck 退出 4 是尚未重放的脏位图，原始失败版本在冷启动后仍不一致，不能记作干净。正例与负例镜像均保留压缩副本和 SHA-256，报告为 `build/evidence/v1-closeout-20261116/ext4-journal-io-flush-matrix/report.json`；阶段修复尚不足的失败尝试保留在 `ext4-journal-io-phase-fixed`。普通崩溃切点修复后报告为 `ext4-journal-crash-checkpoint-fixed/report.json`。

日志模式只有 **ordered 元数据日志**一种（revoke 记录已实现并在重放时生效）。改变日志语义的挂载选项——`data=journal`、`data=writeback`、`journal_async_commit`、`journal_checksum`、`barrier=0`、`nobarrier`、`fast_commit`、`journal_dev`——会被 `ext4_validate_mount_options()` 显式拒绝（`-EOPNOTSUPP`），不会静默降级成 ordered；日志 superblock 侧也只接受 `REVOKE|64BIT|CSUM_V3`。

带日志镜像的日志必须是 v3 校验和格式（由镜像工具显式初始化，不能把 mke2fs 默认格式记作 v3）。旧式 v1/v2 日志不支持重放或写入：干净的 v1/v2 镜像可以**只读**挂载（显式降级），日志非空的旧式镜像连只读都拒绝，可写挂载一律拒绝。`smoke-ext4-legacy-journal` 覆盖这三种情形，并在宿主机上对干净镜像跑 `e2fsck -fn`。

`tools/fs/ext4_feature_gate.py` 已在 RV64 SMP2 上验证外部 e2fsprogs 1.47.2 镜像：干净的 `ORPHAN_FILE` compat 位可接受；`ORPHAN_PRESENT` ro-compat 位和独立的加密 incompat 位都显式拒绝，拒绝镜像不变。现代 orphan-file 重放仍不受支持。

## 2026-11-16 ordered 日志验收（N14）

N14 已关闭。修复后的 RV64 SMP2 记录覆盖四个关键崩溃切点与正常控制、持续 ordered flush/descriptor/payload/commit/checkpoint 错误和同盘冷恢复。C 路径保留失败后的事务阶段；kernel cache 保持 holds 到 checkpoint 写入与 flush 成功。Rust UFSD cache 的 release 操作会另存 checkpoint 快照，共用 journal 路径保留该适配器的快照准备顺序；双方的顺序/失败重试正反 host 用例与 `test-ufsd-cache-host.sh` 均通过。

`tools/fs/ext4_revoke_gate.py` 的六次自然退出 guest 覆盖无 revoke、同事务 revoke、后续事务 revoke、较旧 revoke、32 位 sequence 回绕，以及越界 target 的拒绝。五个合法输入先由独立 e2fsprogs 1.47.2 冷恢复交叉核对，guest 的目标块与其逐字节一致、replay 计数正确、postimage e2fsck 干净；非法输入被明确拒绝且镜像不变。记录为 `build/evidence/v1-closeout-20261116/ext4-revoke-cold-final/report.json`。

不支持的 `journal_dev=...`/`journal_path=...` 参数现在按 key/value 拒绝，避免静默改用内部日志；HAS_JOURNAL 却使用外部设备或缺少内部 inode 的镜像也明确拒绝。扩展的外部格式门禁完成五种状态，拒绝镜像 SHA-256 不变，报告为 `ext4-feature-source-guard/report.json`。旧格式门禁改为按实际 VirtIO slot/唯一容量找盘，保留自然关机检查；干净 legacy v1 只读可读、写入 EROFS、可写挂载拒绝、脏 legacy 日志拒绝及 e2fsck 均通过，报告为 `ext4-legacy-final/report.json`。Rust record/transaction/legacy-policy host 回归通过。

这些记录完成现有 ordered/JBD2 的有界提交、重放、revoke、设备错误恢复和格式拒绝契约；正常盘服务死亡/重挂载验收见下文。

## NTFS 限制

- 不支持 `$ATTRIBUTE_LIST` 跨记录属性（大文件/碎片文件可能拒绝读取）。
- 创建的文件分配在 MFT 空闲记录（首适应，从记录 12 起扫），不维护 `$MFT` 位图。
- 时间戳未按 FILETIME 格式转换（写 0，读时忽略）。
- 不支持 hard link、symbolic link、`$LogFile` replay 或 `$MFTMirr` 恢复；崩溃一致性不等同 Windows NTFS。
- 服务崩溃后的重挂载是可用的：`UXFS_NTFS` 用 SIGKILL 杀掉 ufsd 实例、卸载、重启同一卷，断言内容（8 KB 非驻留文件、嵌套目录、重启标记）仍在。让这一步成立的是 UXFS 卸载路径：服务被 kill 后它再也不会碰通道，`peer_closed` 因此一直是 0，卸载会等满 5 s 超时并返回 `ETIMEDOUT`；现在卸载在超时后用服务任务是否存活再判一次，确认已退出才撤销注册。
- 这里保证的是**服务级**崩溃（进程死掉）后的重挂载；机器掉电不会重放 `$LogFile`，那种情况下卷可能需要 Windows 的 chkdsk，本驱动不声称能修复。
- VFS xattr 仅使用易失的全局 RAM 表，没有 NTFS attribute 持久化后端。

### 硬链接、打开后删除与 Native 临时文件

2026-11-16 修复 Native GETATTR 固定返回链接数 1，以及 READ/WRITE/READDIR 遗漏释放临时 vfile/vnode 的问题。GETATTR 使用带标签的链接数字段传递真实的 2→1→0；旧服务未提供标签时保持原有兼容行为。临时 close 在文件系统私有 context 释放后，成对释放 vnode 与 vfile，避免文件引用持续持有父目录。

`tools/fs/ext4_orphan_lifecycle_gate.py` 对内核 ext4 与 Native 配对 C/Rust orphan 控制器分别验证无日志/JBD2 两种镜像，每种三轮：硬链接、独立 open/dup、最后 unlink 后读写、同名新 inode、truncate、fork 持有至最后关闭和目录删除。Native 通过文件系统类型及独立磁盘 token 证明实际测试目标；卸载后自然关机，再由外部 e2fsck/debugfs 验证。报告见[本轮记录](../../archive/v1-closeout-2026-11-16.md#ext4-硬链接与临时文件引用)。服务死亡/重挂载另见下节；正常生命周期证据不替代崩溃/I/O 失败矩阵。

### Native 服务死亡与重复重挂载

2026-11-16 修复死服务仍等到 RPC 超时、失败 CLOSE 持有本地挂载引用，以及根 vnode/缓存页阻塞 UXFS 槽复用的问题。发送和等待回复都检查被挂载持有 proc 引用保护的服务任务状态；死亡错误保留为 EIO，fd 已消费为 EBADF，本地 fctx/挂载引用可以退休，不伪造 ACK。活服务的失败仍保留重试/失败关闭语义。根 factory 引用交给 mount 持有一次，成功卸载时只释放一次；外部引用仍使旧 root/sb 存活。解绑已撤销旧服务的块 I/O 权限后，清理退休身份的非固定缓存页；映射/外部引用不提前释放。

公开入口 `tools/fs/ext4_orphan_lifecycle_gate.py --service-death --cycles 8`，guest 为 `ufs_orphan_recovery_test`。C/Rust 控制器在无日志/JBD2 各连续八轮服务 SIGKILL、旧 fd EIO/EBADF、卸载、重挂载、已链接文件读回与新写入，冷启动内各完成 16 次挂载，超过静态 8 槽容量。32 次恢复与 64 次挂载全部通过，外部 e2fsck 为 0；正常硬链接生命周期在内核及 Native C/Rust 三条路径另各验证两种格式三轮。详见[本轮证据](../../archive/v1-closeout-2026-11-16.md#ufsd-服务死亡与重挂载)。此结果只覆盖上述正常盘服务恢复场景。

### 位图恢复写的有界重试

2026-11-16 修复无日志 ext4 的位图释放失败后，恢复写也失败一次就立即判为不可恢复的问题。持有 alloc_lock 时重写同一个分配字节可重复执行；现在恢复写最多再试一次，成功仍向调用者返回原 EIO，由原有 orphan 重试路径完成清理。journal 路径继续按既有 poison/replay 处理。持续失败仍进入 EUCLEAN；已隔离文件系统的 create 也直接返回 EUCLEAN，避免误报 ENOSPC。

入口 `EVIDENCE_DIR=<目录> python3 tools/host-tests/test-ext4-orphan-compensation-host.py`，直接链接共享 C ext4 和生产 Rust orphan、extent、cache，操作真实 e2fsprogs 镜像，不构建或启动 guest。第 5/6 次写失败的原版返回 117 且 fsck 报位图差异；修复后返回 EIO，后续清理成功、fsck 0。第 5/6/7 次写均失败时仍持久标记 ERROR_FS，拒绝 create，新进程重新挂载也拒绝且目标镜像不变。故障盘 fsck 非零是保留的隔离状态，不宣称修复。该针对性结果只覆盖所述后端和故障注入范围。

### 目录计数失败与发布前清理

目录计数更新现在返回错误并恢复原计数，不再吞掉组描述符写入失败。`mkdir` 在名字发布前失败时，清除尚未发布的 inode 镜像、撤销目录计数并交还块/inode；`rmdir` 在删除名字前先检查计数更新，计数写失败保留原目录。名字可能已经修改而回滚状态不确定时，保留可能仍被引用的分配并进入 EUCLEAN，避免释放一个仍被目录项指向的 inode。目录延迟释放现已统一到磁盘 orphan 路径；普通 I/O 失败保留链条供重试或挂载恢复，补偿不确定时仍进入 EUCLEAN。

针对性入口为 `EVIDENCE_DIR=<目录> python3 tools/host-tests/test-ext4-orphan-compensation-host.py --directory-account`。同一真实无日志镜像验证 mkdir 计数写失败、已初始化目录块写失败、rmdir 计数写失败；核对名字、空闲块和空闲 inode 回到相应原值，重试与卸载后独立 e2fsck 为 0。最后释放的只读预检查失败与保留拥有者的重试由现有 `test_ext4_orphan_preflight` 针对性检查覆盖。本轮没有执行全量回归或扩大到 journal/Native ABI 崩溃认证。

### 错误状态刷盘重试

无日志错误标记的 C/Rust 状态机现在对相同的 ERROR 状态也执行 checked flush，避免把缓存中的相同字节当成上一次刷盘成功。`ext4_record_status` 对错误标记的写入/刷盘失败只重试一次；无论重试结果如何，本次操作仍返回 EUCLEAN，挂载仍保持不一致状态。若设备持续拒绝写入，不能保证错误标记已经持久化。

2026-11-16 源码提交 `309a52337`。针对性检查区分缓存状态和持久状态，覆盖失败的 barrier、相同状态重试、一次有界补偿及补偿耗尽后再次报告错误；C 检查 1 项、共享 Rust 状态模块 8 项通过，记录在 `build/evidence/v1-closeout-20261116/error-marker-retry/report.json`。这不扩大为 Native ABI 或系统崩溃验收。

### 挂载失败后的日志回调清理

日志已经打开后，组描述符校验、分配提示、orphan 恢复、根 inode 读取或根 vnode 创建失败，现在统一先关闭日志，再撤销块缓存同步回调并释放文件系统上下文。关闭失败时保留日志及其上下文，防止后续 sync 访问已释放对象或绕过失败的事务。

2026-11-16 源码提交 `20dfa2781`。一个真实 JBD2 镜像场景覆盖分配提示 OOM、根 vnode OOM、失败后的 sync/重挂载，以及日志关闭失败时保留有效 owner 并返回 EUCLEAN；外部 e2fsck 0。入口为 `tools/host-tests/test-ext4-orphan-compensation-host.py --mount-abort`，报告在 `build/evidence/v1-closeout-20261116/mount-abort/verified/report.json`。关闭持续失败后的上下文回收与系统崩溃恢复仍不能据此宣称完成。

### 仍被引用的已删除目录恢复

2026-11-16 源码提交 `84cdd129c`。移除仅在 vnode 保存的 `dir_removed` 状态与不返回截断错误的目录专用回收分支。C/Rust orphan 控制器现在分别支持普通文件的最后一个链接和空目录的两个链接，一起复用磁盘链登记、checked truncate、失败重试和挂载恢复。目录项删除状态不确定时仍隔离；向已删除但仍被持有的目录添加子项直接返回 ENOENT。

针对性入口 `tools/host-tests/test-ext4-orphan-compensation-host.py --directory-orphan`，C 控制器加 `--c-orphan`。每条控制路径只运行一个真实无日志镜像场景：目录块释放注入一次 EIO 后重试成功；另一个已删除目录在仍有 vnode 引用时刷盘，后端直接退出，新进程挂载回收，空闲块/inode 恢复原值，外部 e2fsck 0。报告在 `build/evidence/v1-closeout-20261116/directory-orphan/{c,rust}/report.json`。共享的 preflight/vnode publication 两项现有检查通过；没有追加完整回归。这里验证了操作完成并刷盘后的进程退出恢复，不覆盖操作中任意写入切点的崩溃原子性，也不替代 Native 协议或系统重启验收。

### Orphan 链修改与 vnode 归属

2026-11-16 源码提交 `dd69d7bb9`。移除用 `i_dtime`/next 快照作为 vnode 身份的判断：删除后继节点会改变它，inode 复用也可能再次得到同一个 next。挂载实例改用不额外分配内存的 vnode 归属链；成功释放 inode 时撤销归属，最后一次 vnode 释放无论回收结果如何都会从该链摘除，失败的磁盘 orphan 继续留给恢复。共享 C 回调同时覆盖 C/Rust 控制器与挂载/关机恢复。

只复用一个真实镜像场景增加链尾先回收和 inode 编号复用检查：先关闭链尾后关闭链头，orphan 链应立即清空；恢复释放旧 inode 后新对象复用同一编号，旧 vnode 不得回收新对象。其后的目录 EIO 重试/新进程恢复及最终 e2fsck 0 仍通过，报告在 `build/evidence/v1-closeout-20261116/orphan-vnode-owner/verified/report.json`，日志含 `ORPHAN_OWNER_ORDER_REUSE_PASS`。preflight/vnode publication 两项相关检查通过；未重复控制器矩阵或完整回归。

### Inode 释放失败后的可重试镜像

2026-11-16 源码提交 `5856cb751`。最后回收先清空 inode、再释放 inode 位图；位图释放可恢复失败时，原控制器只恢复 links/next，导致 mode 与空 extent 头丢失，下一次 checked truncate 无法识别该 inode。无日志路径现在先恢复截断后、清空前的完整 inode 镜像，再交还控制器恢复 orphan 链；恢复写失败仍进入 EUCLEAN。日志路径保留 poison/replay 规则。

仅在已有真实镜像场景加入一次 inode 位图释放 EIO：返回 EIO 后目录类型和空 extent 头仍在，pending 状态保留，重试成功。原有归属顺序/编号复用、目录块 EIO 重试及新进程恢复均继续通过，最终 e2fsck 0。记录在 `build/evidence/v1-closeout-20261116/orphan-inode-release/verified/report.json`，日志含 `ORPHAN_INODE_RELEASE_RETRY_PASS`。未重复全量回归或控制器矩阵。

### Ext4 最后 unlink 的目录写补偿

2026-11-16 提交 `288c994ca`。最后链接 unlink 在写入 zero-link orphan 后，目录项删除写失败原先只调用 journal poison，无日志挂载不会恢复原名对应的链接状态。现在 dir_remove 的失败写恢复同一块的原 inode/rec_len 并重算 checksum，恢复写有一次有界重试；成功补偿后，unlink 先还原完整的有链接 inode，再还原旧 orphan 链头，返回原可重试错误。任何补偿耗尽则记录 EUCLEAN/ERROR_FS，不继续回收可能仍有名字的 inode。日志挂载仍沿用 poison 管线。

仅运行 `test-ext4-orphan-compensation-host.py --unlink-rollback` 的一项定向真实镜像检查，使用生产 C ext4 与 Rust orphan/extent/cache。单次目录写失败后检查原名、同 vnode、链接数、内容与链头，重试删除并卸载后 e2fsck 退出 0；目录写及两次恢复写均失败时，create 被 EUCLEAN 拒绝，ERROR_FS 持久化，新后端进程拒绝且镜像不变。此隔离盘 e2fsck 退出 4，需要修复，未当作恢复成功。报告在 `build/evidence/v1-closeout-20261116/ext4-unlink-rollback/verified/report.json`。本项没有执行完整 Native ABI/系统断电验证。

## Ext4 非最后硬链接 unlink 的写失败补偿

2026-11-16 提交 `09240ab90`。非最后硬链接 unlink 先更新链接数，再删除目录项；无日志 inode 写失败恢复完整原 inode，后续目录项删除失败且名字已恢复时也还原原链接数。补偿写有一次有界重试，耗尽则持久标记 EUCLEAN/ERROR_FS；日志路径仍 poison。最后链接分支复用同一 inode 恢复 helper。

只运行 `test-ext4-orphan-compensation-host.py --hardlink-unlink`。真实镜像上分别注入计数写失败和随后目录项写失败，原名字、同 vnode、链接数与内容保留，重试删除并卸载后 e2fsck 退出 0。连续计数/补偿失败返回 EUCLEAN，新建拒绝，新后端进程拒绝挂载且镜像不变；该隔离镜像 e2fsck 退出 0 不替代系统恢复验收。报告在 `build/evidence/v1-closeout-20261116/ext4-hardlink-unlink/verified/report.json`。本项只覆盖所述硬链接删除补偿。

## Ext4 硬链接创建与目录块发布补偿

2026-11-16 提交 `2077670d3`。硬链接创建现在保存完整原 inode；计数写失败或未发布名字的后续失败会检查并恢复原计数/ctime，补偿耗尽进入 EUCLEAN/ERROR_FS，拒绝继续修改。零链接/已删除对象不能重新挂名，16 位链接计数饱和返回 EMLINK。已有目录块在空槽或 slack 插入前保存完整原块，写失败有界恢复并重新计算 checksum。扩容先初始化私有新块，再修改映射；初始化失败检查归还分配。映射修改失败保留可能被引用的分配并隔离，父目录 inode 写失败不再吞错；这两类仍需完善恢复，未作为完成项删除。

只运行 `test-ext4-orphan-compensation-host.py --hardlink-create` 一项定向检查，使用共享生产 C ext4 与 Rust cache/orphan/extent 后端。计数写失败、目录块发布失败恢复原名/计数/ctime；填满首个目录块后，新块初始化失败归还空闲块并保持原映射，随后重试扩容、硬链接读取与删除成功，卸载后 e2fsck 退出 0。目录已恢复但计数两次补偿均失败时返回 EUCLEAN，新建拒绝，ERROR_FS 持久化，新后端进程拒绝挂载且镜像不变；隔离镜像 e2fsck 退出 0。报告在 `build/evidence/v1-closeout-20261116/ext4-hardlink-create/verified/report.json`。没有追加全量、guest 或控制器矩阵。

## Ext4 目录扩容的映射与 inode 写失败恢复

2026-11-16 提交 `3e54df733`。新增单次块映射插入的补偿范围：保存原 inode、实际修改的已有 extent/indirect 块，以及本次新分配的映射块。失败时有界恢复映射块和原 inode，全部成功后才归还新分配；不会按旧 EOF 截断并误删既有预分配。无法恢复则持久隔离并保留可能被引用的分配。indirect 增长检查读写错误、初始化新指针块，并拒绝覆盖已有映射。目录新增块的映射与新大小由同一入口写入；create/link/mkdir/symlink/rename 不再另行执行目录增长写，成功后统一更新缓存目录大小。mkdir 的额外父目录链接计数写仍有独立失败恢复工作。

仅运行 `test-ext4-orphan-compensation-host.py --directory-grow`。真实镜像检查 depth-1 extent 叶写失败、叶更新后 inode 写失败，原叶/映射/大小恢复，EOF 之外预分配保留，私有块可释放且空闲计数一致；填满目录后，硬链接触发扩容的父 inode 写失败恢复名字/计数/目录大小与分配，重试扩容及读取成功，卸载后 e2fsck 退出 0。叶写和两次补偿均失败时返回 EUCLEAN，新建拒绝，ERROR_FS 持久化，新后端进程拒绝挂载且镜像不变；隔离盘 e2fsck 退出 4，仍需要修复。报告在 `build/evidence/v1-closeout-20261116/ext4-directory-grow/verified/report.json`。该定向检查不证明任意无日志崩溃切点或完整 Native 系统重启恢复；未追加全量或配置矩阵。

## Ext4 创建操作的私有对象与父目录计数恢复

2026-11-16 提交 `0bf77174d`。普通文件/FIFO、快速符号链接与 mkdir 统一使用检查返回值的私有对象清理：清除 inode，撤销已登记的目录计数，归还块和 inode；无日志清理有一次有界重试，任何步骤不确定则停止后续释放并持久隔离。符号链接不再忽略初始 inode 写失败；普通文件所需 vnode 在名字发布前准备，分配失败不会留下已创建的名字。mkdir 在发布名字前检查父目录链接数写入，失败恢复原 inode；名字发布失败时撤销已增加的父计数，同时保留成功的 HTree 线性化结果，避免重新设置旧 INDEX_FL。父计数饱和返回 EMLINK。

只运行 `test-ext4-orphan-compensation-host.py --create-rollback` 的定向真实镜像检查：文件/符号链接初始化与名字发布失败、mkdir 父计数写失败及名字发布失败，均保留名字缺失、原父计数、目录总数和空闲块/inode 数；重新创建、读取符号链接、删除和卸载成功，e2fsck 退出 0。初始 inode 写及两次清除均失败时返回 EUCLEAN，inode 分配仍保留，新建拒绝，新后端进程拒绝挂载且镜像不变；该隔离盘 e2fsck 退出 4，仍需修复。报告在 `build/evidence/v1-closeout-20261116/ext4-create-rollback/verified/report.json`。未运行全量或配置矩阵。



### 目录移动与安装后的清理（2026-11-16）

修复跨父目录 rename 缺失的磁盘 `..`、父目录链接数和缓存 parent；目录替换使用两链接 orphan 清理，exchange 处理两侧目录归属，拒绝移入自身子树，同 inode 硬链接 rename 保持 no-op。RV64 SMP2 定向功能检查自然关机、FAT/ext4 检查 0；此前旧内核复现父目录计数错误、清理 EIO，e2fsck 退出 4。真实 Python 离线构建/安装/导入与全部清理也通过并自然关机，磁盘检查 0。证据在 `build/evidence/v1-closeout-20261116/directory-rename/`，详细范围见[本轮归档](../../archive/v1-closeout-2026-11-16.md#跨父目录-rename-与-python-安装清理)。本项未扩大失败注入；npm 负载的修复见下节。


### 普通数据块与 npm 写回（2026-11-16）

部分新文件块经元数据分配器清零，导致普通数据占满 JBD2 描述符，npm 安装后应用虽通过却无法正常关机。现按普通 ordered data 先组成零填充与有效数据的完整块、写入后发布映射；未扩大日志容量或取消记录失败保护。相同 RV64 SMP2 unix-names/node-install 工作负载修复前重现拒绝，修复后自然关机、ext4/FAT 检查 0，报告在 `build/evidence/v1-closeout-20261116/ordered-file-data/verified/`。已有一项写回宿主检查覆盖零填充、数据先于映射、初始化错误回收及原 orphan 并发保护；本项未扩展失败矩阵。详见[本轮归档](../../archive/v1-closeout-2026-11-16.md#普通文件新块误入元数据日志与-npm-关机)。
