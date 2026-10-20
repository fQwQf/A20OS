# 已退役的 PPC64LE 实例快照

这些 TOML 清单在 `v0.18` 基线时位于 `instances/`。它们作为历史配置事实保留，但当前 `tools/a20` 注册表
不扫描也不执行本目录；`make ARCH=ppc64le ...` 会被显式拒绝。

复现时请用带注释的 `v0.18` 标签的干净 checkout，那里这些文件仍在 `instances/` 下的原始路径。当前 checkout
可能带有本地改动，切换版本前先自行保存。
