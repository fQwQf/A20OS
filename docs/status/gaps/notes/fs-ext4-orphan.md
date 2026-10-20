# ext4 孤儿 inode：unlink 后回收失败

> 来源：原 `docs/status/current/known-issues.md` 的对应小节；该文件已拆分删除。
> 关联台账：[FS-01](../fs.md)。

`ext4_release_vn()`（`kernel/fs/diskfs/ext4_namei.c:717`）在一个「被引用期间被 unlink」的 inode 上调
`ext4_orphan_reap()`，后者返回 `-95`（EOPNOTSUPP），于是打印
`[EXT4] unlink orphan ... deferred orphan cleanup failed ...; orphan retained for recovery`，
inode 留在 orphan 链里。world 镜像无日志（`^has_journal`），`sb->journal` 为 NULL，
journal poison 那一步被跳过，所以当场没有别的报错 —— 但**下一次挂载这张镜像会直接失败**：
`[EXT4] refusing mount: orphan recovery failed (-95)`，镜像被拒，启动退回 FAT32 根，
表现为「world 没生效」。同一张镜像连续启动两次即可复现。

触发面比预期大：zsh 打开 `SHARE_HISTORY` / `INC_APPEND_HISTORY` 时每条命令都会重写历史文件，
实测**每条命令**留一个 orphan（连 `echo` 都算；同一张镜像上 busybox ash 跑同样命令是 0 条）。
关掉这两个选项后 orphan 计数归零，镜像连续两次启动都正常 —— 规避写在
`packages/overlay/cli/root/.zshrc`（HISTFILE 放 /dev/shm，不设那两个选项）与
`packages/world/cli.world`。

平时不易察觉是因为 `a20 run` 每次都会重建 world 镜像（`image-world` 是 phony），
缺口被「重建」掩盖；`a20 run --no-build`、或手工用同一张镜像连续启动两次才会踩到。

## 另一条路径：QEMU 被直接 kill

与 shell 无关：**QEMU 被直接 kill（而不是 guest 自己 `poweroff` 走完）
时，镜像同样可能变成无法挂载**，而且这种情况下串口上一条 `unlink orphan` 都不会出现
（实测：同一次实验里 4 个 boot 的 orphan 计数全是 0，第 5 次却 `refusing mount`）。
所以「镜像脏了」有两个来源 —— 写盘期间被 kill，以及上面那条 orphan 回收缺口 ——
排查时不要只盯 orphan 日志。world 镜像本来就无日志，这也是 `a20 run` 干脆每次重建的原因。
