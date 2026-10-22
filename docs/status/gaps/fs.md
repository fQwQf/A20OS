# FS 缺口

对应 V1-FS。现状见 [current/fs-status.md](../current/fs-status.md)。

### FS-03 ext4 orphan reap 拒绝 depth-1 extent 树，桌面 unlink 留下脏位图

- 现状：干净树上 x86_64 桌面（KVM）完成一轮写负载（300 个文件、缩略图缓存、
  xfconf 改写、JVM/mpv 状态）后用 `poweroff` 正常关机，宿主 `e2fsck -fn` 仍报
  两个 inode 在 orphan 链上、块/inode 位图差异；同一轮串口日志有
  `[EXT4] deferred orphan cleanup failed ino=144819 err=-95`。两个 inode 都是
  镜像里被桌面替换掉的旧缓存文件（其中一个 1.37 MB，`i_links_count=0`），
  extent 树是 depth-1。
- 缺口：`kernel/fs/diskfs/ext4_extent_txn.rs` 的 orphan reap 只接受 depth=0 的
  extent root（`magic != EXT4_EXT_MAGIC || depth != 0` 直接返回 `-EOPNOTSUPP`），
  所以超过根节点容量、需要 depth-1 树的文件在 unlink 后无法回收，位图与
  orphan 链一起留在盘上。桌面写负载（缓存文件、JVM 模块）必然产生这类文件。
- 关闭条件：depth-1 extent 树的 orphan reap 在 main 上可用；桌面写负载 → 正常
  关机 → 宿主 `e2fsck -fn` 干净（与 DESKTOP-07 联动）。
- 来源：DESKTOP 本轮在 `world-work.img` 上的写负载与独立 e2fsck。
