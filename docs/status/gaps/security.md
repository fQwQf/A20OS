# SECURITY 缺口

对应 V1-SECURITY。现状见 [security/hardening.md](../../security/hardening.md)。

### SECURITY-02 安全开关默认值

- 现状：用户态 W^X 默认 `off`（V8 等需要），翻译器路径按任务豁免。
- 缺口：默认关闭意味着"安全模式"不是默认配置，缺少开启状态下的完整验收。
- 关闭条件：安全控制全开时功能矩阵通过，并把性能代价与默认值决策记录下来。
- 来源：[exec-xlator/01-usage.md](../../exec-xlator/01-usage.md) §W^X、[security/kernel-wx.md](../../security/kernel-wx.md)。

### SECURITY-03 seccomp / Landlock / IOMMU 隔离

- 现状：Landlock/seccomp 有 Rust 事务切片；IOMMU 用户态驱动隔离有门禁。
- 缺口：跨 uid/non-owner/no-cap 拒绝、VM/IOMMU 越界拒绝与拒绝无副作用的组合未完整覆盖。
- 关闭条件：每项都有正/负用例，拒绝路径无副作用，资源不泄漏。
- 来源：requirements L 包范围、[current/v1.0-progress.md](../current/v1.0-progress.md)。
