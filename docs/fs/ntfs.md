# NTFS 文件系统

A20OS 内置 NTFS 读/写文件系统（`kernel/fs/ntfs.c`）。实现依据 NTFS 磁盘格式规范独立编写，

## 支持范围

| 能力 | 状态 | 说明 |
|------|------|------|
| 挂载 | ✅ | 解析启动扇区、`$MFT`、`$Bitmap`；支持 512/1024/2048/4096 扇区与常见簇大小 |
| 目录枚举 / 查找 | ✅ | 解析 `$I30` 索引根 + 索引分配块（含 USA fixup、稀疏 run） |
| 文件读 | ✅ | 常驻（resident）与非驻留（non-resident）`$DATA`，稀疏 run 返回零 |
| 文件写 / 截断 | ✅ | 常驻写入、常驻→非驻留转换、经 `$Bitmap` 分配簇并扩展 run list |
| 创建 / 删除 | ✅ | 分配 MFT 记录、构建 `$STANDARD_INFORMATION`/`$FILE_NAME`/`$DATA`/`$INDEX_ROOT`，目录索引增删 |
| 重命名 | ✅ | 移除旧索引项，改写子记录 `$FILE_NAME`（新 parent ref + 新 name），插入新索引项；失败时恢复旧索引项 |
| 压缩 / 加密属性 | ❌ | 读路径显式拒绝（返回 EIO） |
| 目录索引扩容 | ⚠️ | 追加到索引根或已有索引分配块，两者都满时返回 `-ENOSPC`（不做 B-tree 块分裂） |

## 设计要点

- **帧/页面所有权**：与核心 MM 的 VMO/page-cache 语义一致，文件数据通过 `vnode->ops->readpage/writepage`
  接入 page cache，与其它块文件系统（fat32/ext4）同一路径。
- **簇分配器**：`$Bitmap`（MFT 记录 6）首适应分配/释放，写入后回写位图。
- **USA fixup**：MFT 记录与 INDX 块读写均做更新序列数组修复。
- **run list**：解析支持稀疏 run；写入时按相对 LCN 增量编码。

## 挂载

```c
mount -t ntfs /dev/vda2 /mnt
```

在 `vfs_mount_bc` 中注册为 `ntfs`（`FS_TYPE_NTFS`）。无设备探测时的自动挂载回退顺序
为 `ext4 → vfat`，NTFS 需显式指定 `-t ntfs`。

## 限制

- 不支持 `$ATTRIBUTE_LIST` 跨记录属性（大文件/碎片文件可能拒绝读取）。
- 创建的文件分配在 MFT 空闲记录（首适应，从记录 12 起扫），不维护 `$MFT` 位图。
- 时间戳未按 Windows FILETIME 转换（写 0，读时忽略）。
