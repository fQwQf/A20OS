# 用户态文件系统服务（uxfs / ufsd）

本文档描述 A20OS 混合内核的**用户态文件系统层**：内核侧的 `uxfs` 代理文件系统与用户态的 `ufsd` 服务进程。它把"文件系统实现"从内核特权空间迁出，是混合内核"可崩溃、可重启的用户态服务作为系统组成部分"这一设计原则在存储栈上的落地。设计参考：Linux FUSE（内核代理 + 用户态守护）、Fuchsia（文件系统运行在用户态 storage 进程、经块客户端协议访问设备）。

## 定位与边界

```
应用 read()/write()/open() …
  └─ Linux ABI / Native ABI（线格式翻译）
      └─ VFS 核心（内核，保留：路径解析、dentry、fdtable）
          └─ uxfs 代理 vnode ops（内核新增，薄转发层）
              └─ Channel IPC（channel_send/recv，64 KiB 内联载荷）
                  └─ ufsd 用户态服务（FAT32 实现 = fat32lite 同源编译）
                      └─ fs_block_io 系统调用 → 内核块层/页缓存
                          （scratch 盘：udisk 代理 ↔ ubd 用户驱动）
```

按 [00-design.md](00-design.md) 的判定规则：

- **留内核**：VFS 核心、页缓存与块缓存、块设备抽象、驱动框架——高频、延迟敏感；
- **迁用户态**：FAT32 等*文件系统实现*——协议解析类工作、崩溃可恢复、性能非最关键路径。

这与既有决策一致：[02-mainstream-plan.md](02-mainstream-plan.md) 已记录"页缓存与文件系统留内核"针对的是**主存储数据面**；uxfs 面向 scratch/辅助盘，主存储 virtio-blk + 内核 FAT32 路径保持不变，两者并存。

## 组件

### ufsd（user/svc/ufsd.c）——多人格文件系统宿主

- Native ABI 服务进程；一个二进制承载多种文件系统后端，按 argv 选择：
  | fstype | 实现来源 | 语义 |
  |--------|----------|------|
  | `fat`（默认） | fat32lite 同源编译（freestanding，IO 回调注入） | 读写 |
  | `ext4` | 内核 diskfs 源码经 fscompat 兼容环境原样编译 | 读写（含 rename/日志） |
  | `iso9660` | 同上 | 只读 |
  | `ntfs` | 同上 | 读写（内核 ntfs 写路径由 smoke-native-fs-all 在库端到端覆盖，见边界） |
- fscompat（user/svc/fscompat/）：以遮蔽头 + 等价实现为内核磁盘 FS 源码提供用户态运行环境——kmalloc→malloc、锁退化为 no-op（单线程宿主）、bcache 写穿透实现、vnode/vfile 引用助手、恒等 usercopy。FS 源码零改动。
- vnode 型后端维护 ino→vnode* 映射；操作全部经由各 FS 自身的 vnode_ops/vfile_ops 表执行。
- 块 IO 经 `fs_block_io` 原生 syscall 进入内核块层；内核校验只有注册该挂载的服务任务可以发起（含 count==0 的容量查询语义）。
- 服务循环：`channel_recv` 取请求 → 后端执行 → `channel_send` 应答；请求以 `req_id` 匹配，乱序应答丢弃。线上 name/payload 不带 NUL，分发前统一终止化。

### uxfs 代理（kernel/fs/uxfs/uxfs.c）

- 以 fstype `"uxfs"` 注册的 VFS 挂载类型（FS_TYPE_UXFS）。
- 注册 syscall `fs_serve(target_path, server_endpoint, block_index)`：
  1. 校验 endpoint 为 CHANNEL_ENDPOINT 句柄且具备 READ|WRITE 权限；
  2. 解析 block_index 对应 `block_dev_t` 并记录服务任务所有权；
  3. 完成挂载。无同步握手——调用方就是服务进程自身，阻塞等 INIT 会自我死锁；活跃性由首个真实文件操作验证。
- vnode 操作（lookup/create/mkdir/unlink/rmdir/rename/stat/truncate/read/write/readdir/statfs/sync）翻译为 ufs 协议消息；数据内联传输（≤ UFS_MAX_PAYLOAD）。
- 服务死亡时 channel 断链使在飞请求返回 `-EIO`；重新拉起后需重新挂载（与 ubd_recover 的重挂载语义一致）。
- **svcmgr 托管已接入**：清单项 `ufsd args="/ufs 1 fat"`，spawn 支持 argv；ufsd 应答 IDL echo 健康探针；目标块设备缺失时卸载挂载并以 0 干净退出（监管者不计入重启预算）。崩溃恢复由 `smoke-native-fs-all` 的 UXFS_RESTART 段实测：SIGKILL → umount2 → 重启实例 → 数据持久。
- umount 通知路径：内核侧 uxfs_unmount 经 `a20_channel_ep_peer_shutdown` 单向置对端 peer_closed 并唤醒，服务 recv 随即出错退出，不产生僵尸实例。

## 已知边界

- fat 后端：8.3 短名（fat32lite 语义）；rmdir/rename/非零 truncate 不支持（fat32lite 无对应原语，返回 ENOSYS）；
- ntfs 后端已开放读写：内核 ntfs 写路径（MFT 记录分配、$INDEX_ROOT 增长、簇位图、驻留→非驻留转换）由 smoke-native-fs-all 端到端覆盖，含 SIGKILL 重启持久化验证；
- iso9660 只读（格式即如此）；驱动将 ISO 名字转小写；
- uxfs 文件读写已接入内核页缓存（readpage/writepage 缓存路径）：读经 page_cache 命中免 RPC，写缓冲在脏页并在 close/fsync 时经 writepage 批量回传 ufsd（ufsd 可崩溃，write-behind 不越过 close）；只读后端（iso9660 经 fs_serve flags 声明）禁用缓冲写；
- 服务重启后映射清空：崩溃恢复契约要求重新挂载（新 ino 空间）；
- 监管通道为 200µs 非阻塞轮询（与 fs 服务循环复用一个线程）；EventQ 统一等待是后续优化项；
- 内核侧仍保留同名 FS 实现（引导期 /bin 挂载与 EXTERNAL_ROOT 发行版路径依赖它们），当前为双态并存；移除需先完成"用户态先于存储可用"的启动序列改造。

## 验证

```bash
make smoke-native-fs-all   # 四后端端到端 + svcmgr 托管 + SIGKILL 恢复演练
make smoke-native-ufs      # 仅 FAT 后端回归
```
