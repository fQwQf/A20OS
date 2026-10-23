# FS 缺口

对应 V1-FS。现状见 [current/fs-status.md](../current/fs-status.md)。

### FS-SECURITY-03 Landlock 允许目录内的符号链接可读取拒绝目录

- 现状：READ_FILE ruleset 只允许 `/tmp/security-release` 和 `/bin`；直接打开 `/tmp/security-release-outside` 得到 EACCES，但打开 `/tmp/security-release/link`（指向前述文件）成功。
- 缺口：`kernel/fs/vfs.c::vfs_open_impl()` 在路径解析前把原始 path 交给 Landlock；`kernel/ipc/landlock.rs` 做字符串前缀匹配，实际符号链接目标的权限没有被验证。
- 关闭条件：由 FS/IPC owner 在真实解析与访问边界强制规则并审查路径别名/并发语义；`security_release_test` 的 `landlock-symlink` 必须拒绝并保持目标内容不变，允许路径仍能打开；不能只按测试字符串封堵。
- 来源：SECURITY 包 `make smoke-security-release` 的 `landlock-symlink FAIL code=2`（2026-10-23），对应 SECURITY-03；本包未修改 VFS/IPC 源码。
