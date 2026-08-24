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

### ufsd（user/svc/ufsd.c）

- Native ABI 服务进程，由 svcmgr 按清单拉起并监管（崩溃重启）。
- 复用 `kernel/fs/diskfs/fat32lite.c`（freestanding、`fat32lite_io_t` 回调注入 IO）直接编入服务进程——与 drvmod 双态部署同源的代码复用方式。
- 块 IO 经 `fs_block_io` 原生 syscall 进入内核块层；内核校验只有注册该挂载的服务任务可以发起。
- 服务循环：`channel_recv` 取请求 → fat32lite 执行 → `channel_send` 应答；请求以 `req_id` 匹配，乱序应答丢弃。

### uxfs 代理（kernel/fs/uxfs/uxfs.c）

- 以 fstype `"uxfs"` 注册的 VFS 挂载类型（FS_TYPE_UXFS）。
- 注册 syscall `fs_serve(target_path, server_endpoint, block_index)`：
  1. 校验 endpoint 为 CHANNEL_ENDPOINT 句柄且具备 READ|WRITE 权限；
  2. 解析 block_index 对应 `block_dev_t` 并记录服务任务所有权；
  3. 发送 `UFS_OP_INIT` 握手取得根 ino 后完成挂载。
- vnode 操作（lookup/create/mkdir/unlink/rmdir/stat/truncate/read/write/readdir/statfs/sync）翻译为 ufs 协议消息；数据内联传输（≤ UFS_MAX_PAYLOAD）。
- 服务死亡时 channel 断链使在飞请求返回 `-EIO`；svcmgr 重启 ufsd 后需重新挂载（与 ubd_recover 的重挂载语义一致）。

## 已知边界

- 8.3 短名（fat32lite 语义），单线程服务循环；
- uxfs 文件读写当前不接入内核页缓存（直通转发）；接入 readpage/writepage 缓存路径是后续工作；
- 每次 ufsd 重启需要显式重新挂载，无自动重绑；
- 单挂载点、单服务实例。

## 验证

```bash
make smoke-native-ufs   # QEMU 中经 ufsd 完成 open/read/write/listing/unlink 全流程
```
