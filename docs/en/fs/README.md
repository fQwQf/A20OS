# File System (VFS & FS) Documentation Index

A20OS utilizes a virtual file system (VFS) with a layered architecture, where the core page cache and path resolution operations are executed in kernel mode. The implementation of specific file systems can operate in both kernel module form and user-space services (such as ufsd/uxfs).

---

## Core Mechanisms and Specifications

| Document | Description |
|---|---|
| [vfs-edge-semantics.md](vfs-edge-semantics.md) | VFS boundary semantics: path resolution, symbolic link handling, and special files |
| [fs-consistency-model.md](fs-consistency-model.md) | File system consistency and durability models: JBD2 logging, ordered mode, and crash consistency |
| [ufs-close-token.md](ufs-close-token.md) | Close token semantics and ownership transfer in user-space file systems (ufsd) |
| [ntfs.md](ntfs.md) | NTFS read-only driver implementation and attribute parsing |
