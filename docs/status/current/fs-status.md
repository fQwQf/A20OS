# 文件系统当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

各文件系统后端的当前支持范围与限制。

<!-- 以下内容从 docs/fs/ntfs.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## 限制

- 不支持 `$ATTRIBUTE_LIST` 跨记录属性（大文件/碎片文件可能拒绝读取）。
- 创建的文件分配在 MFT 空闲记录（首适应，从记录 12 起扫），不维护 `$MFT` 位图。
- 时间戳未按 FILETIME 格式转换（写 0，读时忽略）。
- 不支持 hard link、symbolic link、`$LogFile` replay 或 `$MFTMirr` 恢复；崩溃一致性不等同 Windows NTFS。
- VFS xattr 仅使用易失的全局 RAM 表，没有 NTFS attribute 持久化后端。
