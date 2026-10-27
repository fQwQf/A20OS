# 用户态 submodule

首次检出或切换版本后执行：

```sh
git submodule update --init --recursive
```

依赖以 gitlink 的完整 commit 固定；不要用 `git submodule update --remote` 替代版本检出。

| 组件 | 当前来源 | 固定 commit |
| --- | --- | --- |
| musl | [A20 fork](https://github.com/fQwQf/musl) | `9a31351229ad9c4c95119a6a76fd1e3280ffa45d` |
| sbase | [upstream](https://git.suckless.org/sbase) | `c1341583c96307cb0e6152c963ed23c4d56a4278` |
| mksh-cvs2git | [upstream](https://github.com/mirabilos/mksh-cvs2git) | `341752823184238dc9b2b0a22b48da984f25442d` |
| mlibc | [A20 fork](https://github.com/fQwQf/mlibc) | `3a5f8bbcef3c9906dde934d5862e8eed8cb854f5` |
| tlse | [upstream](https://github.com/eduardsui/tlse) | `e69790bb14e35e2db6683f6193cb2b685780f7aa` |

## 集成约定

- musl 保留 RISC-V32 syscall 别名、四个调度 API 的 syscall 绑定和 `getifaddrs` 的 A20 fallback。原版 `getifaddrs` 在当前内核返回 EINVAL，原版调度 API 返回 ENOSYS；不能整体换成原版。
- mlibc 的 `.github/workflows` 保持固定上游基线配置，不导入 A20 的 CI 改动。Native 构建源码不受此例外影响。
- mlibc 保留 Native ABI 的 `sysdeps/a20` 及运行库、线程、loader 适配。Linux ABI 的修复不会替代这套 Native ABI 实现。
- mksh 使用原版上游源码；配置和生成头文件位于 `user/ports/mksh`。musl 和 mlibc 两套构建都必须包含该目录。
- sbase 使用原版上游源码；项目原有的额外 `id` 命令放在 `user/ports/sbase`。其 MIT LICENSE 随源码保留。
- TLSe 使用原版上游 snapshot，移除原来的 A20 fast-math 和 cipher 补丁及相关宏；其余原有 TLS 构建选项保持不变。

## 历史版本

历史依赖仓库使用 `a20/v0.1` 至 `a20/v0.18` 标签固定各版本源码；某组件未出现的版本没有对应标签。源码不变时多个标签共用一个依赖 commit。`a20/current` 保留迁移前 HEAD 的构建源码状态（mlibc CI 使用上游配置）；当前 gitlink 可以在验证后采用较小适配或原版上游。

历史迁移只保证各版本 tag 与迁移前 HEAD 的依赖构建源码准确还原；mlibc 的 `.github/workflows` 统一保留上游配置。版本区间内的普通提交使用累计版本 snapshot，不保证逐提交源码还原。历史内核、外部工具链和已有其他 submodule 的可用性仍取决于原版本；本次迁移没有修复它们的既有缺陷。

## 验证

升级接受门禁为 `make check-upgrade-userland-smokes`。本次同时在 RISC-V64 和 AArch64 验证 mksh 脚本/source/管道、sbase id、musl 网络接口和调度 API、原版与补丁版 TLSe 的 TLS 1.2 RSA/AES128-SHA HTTPS 下载及网络套件。测试不构成对所有 TLS cipher、架构或历史内核的覆盖。

迁移前 HEAD 的 RISC-V generic driver 构建缺少 `riscv_dma_sync_device/cpu` 导出，启动时无法加载块驱动；运行验证采用了工作区已有的对应导出修复作为独立测试 overlay。依赖迁移提交不包含该内核修复。
