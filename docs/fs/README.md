# 文件系统（VFS & FS）文档索引

A20OS 采用虚拟文件系统（VFS）分层架构，核心页缓存与路径解析运行于内核态，具体文件系统实现支持以内核模块或用户态服务（ufsd/uxfs）双重形态运行。

---

## 核心机制与规范

| 文档 | 说明 |
|---|---|
| [vfs-edge-semantics.md](vfs-edge-semantics.md) | VFS 边界语义规范：路径解析、符号链接循环与特殊文件处理 |
| [fs-consistency-model.md](fs-consistency-model.md) | 文件系统一致性与持久性模型：JBD2 日志、ordered 模式与 crash consistency |
| [ufs-close-token.md](ufs-close-token.md) | 用户态文件系统（ufsd）Close Token 语义与所有权流转 |
| [ntfs.md](ntfs.md) | NTFS 只读驱动实现与属性解析 |
