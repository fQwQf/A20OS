# 开发者指南文档索引

本目录面向 A20OS 内核与系统开发者，提供从环境配置、编译构建、实例声明、运行调试到工具链内部实现的完整技术指南。

---

## 开发与调试指南

| 文档 | 说明 |
|---|---|
| [build.md](build.md) | 完整构建与运行指南（依赖安装、交叉编译器、QEMU 启动与故障排查） |
| [instances.md](instances.md) | 实例化声明系统（`instances/*.toml` 与 `tools/a20` 统一入口） |
| [debug.md](debug.md) | 内核调试实践指南（GDB 附加、断点设置与 QEMU 调试接口） |
| [boot-model.md](boot-model.md) | 系统启动模型与硬件探测流程（Multiboot2、FDT/DTB、UEFI） |
| [external-dependencies.md](external-dependencies.md) | 外部依赖项与第三方库准入规范 |
| [tooling-internals.md](tooling-internals.md) | 构建系统与宿主工具技术内幕（`tools/` 脚本原理解析） |
| [userland-submodules.md](userland-submodules.md) | 用户态子模块（Alpine rootfs、musl、ports）同步与管理 |
