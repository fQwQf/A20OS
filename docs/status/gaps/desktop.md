# DESKTOP 缺口

对应 V1-DESKTOP。现状见 [current/desktop-minecraft-status.md](../current/desktop-minecraft-status.md)；
仍未关闭问题的诊断见 [notes/](notes/README.md)。

### DESKTOP-06 Minecraft 性能

- 现状：854×480、4 区块视距下约 8–18 FPS，菜单约 20 FPS。
- 缺口：未达到稳定 30/60 FPS，也没有性能目标与对照。
- 关闭条件：给出目标帧率与同输入对照，达到目标或把目标改为有依据的契约。
- 来源：[current/desktop-minecraft-status.md](../current/desktop-minecraft-status.md)。

### DESKTOP-07 桌面扩展写盘一致性

- 现状：x86_64 桌面上完成一轮写负载（300 个文件、缩略图缓存、xfconf 改写、
  JVM/mpv 状态）并正常关机后，宿主独立 `e2fsck -fn` 仍报两个 inode 在 orphan
  链上、块/inode 位图差异；日志有 `deferred orphan cleanup failed ... err=-95`。
  根因是 ext4 orphan reap 不接受 depth-1 extent 树，桌面 unlink 的大文件无法回收，
  已记为 [FS-03](fs.md)，不在本包修复范围。
- 缺口：guest 写盘后的 ext4 位图一致性仍需按历史触发场景复验；孤儿回收失败的
  归属在 FS-03。
- 关闭条件：桌面写负载后关机再 `e2fsck` 干净；与 FS-01/FS-02/FS-03 联动。
- 来源：原 `known-issues.md` 当前工程队列；本轮 `world-work.img` 复验。

### DESKTOP-14 `svcmgr` 强制终止在 AArch64 的验收

- 现状：RV64 dev 镜像上已验证健康超时 → `sig=9` 强停 → 恢复，含负控与 shutdown 审计；AArch64 桌面门禁未复跑。
- 缺口：同一路径在 AArch64 上没有验收；历史上该板的 ufsd 健康探测超时也未解释。
- 关闭条件：AArch64 桌面实例上完成 HANG 注入、强停、恢复与负控，审计干净。
- 来源：原 `known-issues.md` 当前工程队列。
