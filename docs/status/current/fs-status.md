# 文件系统当前状态

> 仍未完成的事项见 [1.0 缺口台账](../gaps/README.md)；本页只描述当前状态。

各文件系统后端的当前支持范围与限制。

<!-- 以下内容从 docs/fs/ntfs.md 迁入；只保留当前状态，历史运行记录已删除。 -->

## ISOFS

只读；名字按 ISO 9660 规则转小写并去掉 `;1` 版本后缀。没有 Rock Ridge / Joliet。

测试镜像由 `tools/mkisofs_test.c` 生成，刻意覆盖这几类形状：跨多个 2048 字节逻辑块的根目录
（记录不跨块，块内剩余部分补零，扫描需要续到下一块）、200 字节长名（贴近驱动
`ISOFS_MAX_NAME` 上限）、零长文件、恰好一个块的文件、跨块文件，以及嵌套目录。
`smoke-vfs-stress` 在 guest 内读回全部这些文件（含首块与末块目录项），并检查 EOF 之后
读返回 0、缺失名字返回 `ENOENT`、写打开失败。同一张镜像也供用户态 ufsd 的
`smoke-native-fs-all` 使用。

文件读取走页缓存；`readpage` 的返回值是页内有效字节数，页缓存会把该值之后的部分清零。

## NTFS 限制

- 不支持 `$ATTRIBUTE_LIST` 跨记录属性（大文件/碎片文件可能拒绝读取）。
- 创建的文件分配在 MFT 空闲记录（首适应，从记录 12 起扫），不维护 `$MFT` 位图。
- 时间戳未按 FILETIME 格式转换（写 0，读时忽略）。
- 不支持 hard link、symbolic link、`$LogFile` replay 或 `$MFTMirr` 恢复；崩溃一致性不等同 Windows NTFS。
- VFS xattr 仅使用易失的全局 RAM 表，没有 NTFS attribute 持久化后端。
