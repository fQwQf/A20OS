# 包与 recipe 参考

最后核实：2026-08-27（与 `tools/mka20pkg.py` 同步维护）。

A20OS 的包 = **标准 apk v2 包**。打包器 `tools/mka20pkg.py` 读取一个
TOML recipe，把构建产物收集成包。recipe 是纯数据文件，不含构建逻辑——
**构建仍由 Makefile 完成，recipe 只负责"把已构建的产物装进包"**。

## recipe 完整示例与字段参考

```toml
[package]
name = "a20-base"          # 必填。包名，apk 世界里的唯一标识
version = "0.2.0"          # 必填。上游/项目版本号
release = 0                # 可选，默认 0。同一版本的打包修订号，
                           #   完整版本 = {version}-r{release}
description = "..."        # 必填。一句话描述（apk 的 pkgdesc）
license = "Apache-2.0"     # 必填。SPDX 标识
url = "https://..."        # 可选
maintainer = "..."         # 可选
origin = "a20-base"        # 可选，默认同 name。同源码衍生的包共享 origin
archs = ["riscv64", ...]   # 可选。缺省表示全架构；列出则为架构白名单
depends = ["musl"]         # 可选。apk 依赖（名字或 name=version）
provides = []              # 可选。虚拟提供（provides）

[vars]                     # 可选。自定义占位符（见下表）
foo = "bar"

[[files]]                  # 文件规则，可多个；三种形式见下
src = "{build_dir}/*"
dest = "/"
exclude = ["*.o"]
```

## 文件规则（`[[files]]`）的三种形式

### 1. `src` —— 从构建产物/源码树收集文件

```toml
[[files]]
src = "{build_dir}/*"        # 支持 glob；含 ** 时递归
dest = "/"                   # 包内绝对路径；以 / 结尾表示目录（按文件名放入）
mode = "0755"                # 可选；缺省保留源文件权限
optional = true              # 可选；匹配不到时跳过而不是报错
exclude = ["*.o", "*.a"]     # 可选；按文件名 fnmatch 排除
```

行为细节（与旧镜像规则对齐）：

- 非递归 glob（如 `*`）匹配到**目录时静默跳过**——与旧 Makefile 里
  `[ -f "$f" ] || continue` 的语义一致；
- 递归 glob（含 `**`）会**保持目录结构**：dest 为目录时，包内路径 =
  dest + 源路径相对 `**` 前基路径的部分。例：
  `src = "{repo}/user/external/apps/vim/runtime/syntax/**"` +
  `dest = "/extra/share/vim/vim92/syntax/"` 会把整棵 syntax 树原样落位；
- 多条规则匹配同一文件时后者覆盖前者（tar 内同名后写覆盖）。

### 2. `content` —— 内联文本文件

```toml
[[files]]
dest = "/etc/os-release"
mode = "0644"
content = """
ID=A20OS
VERSION="{version}"
"""
```

用于 os-release、配置文件等"本来就该由包管理"的小文件——旧流程里它们
是 Makefile 里 `printf` 塞进去的，现在有了归属。

### 3. `symlink` —— 符号链接

```toml
[[files]]
dest = "/sh"
symlink = "/mksh"
```

## 占位符

`src`、`dest`、`content`、`symlink`、`[vars]` 值中可用：

| 占位符 | 展开为 |
|--------|--------|
| `{arch}` | 目标架构（apk 语义，如 `riscv64`） |
| `{variant}` | 用户态构建变体（`riscv64` 或 `riscv64-nommu`） |
| `{build_dir}` | `user/build/{variant}` |
| `{extra_dir}` | `user/build/extra/{arch}` |
| `{kernel_build}` | 内核构建目录（make 集成时传入 `.kernel-build/...`） |
| `{repo}` | 仓库根目录 |
| `{name}` / `{version}` | 本包的 name / version |
| 自定义 | `[vars]` 表与命令行 `--set key=value` |

## 命令行

```bash
# 打包（产物在 build/packages/<arch>/）
tools/mka20pkg.py packages/recipes/a20-base.toml --arch riscv64 \
    --sign-key build/keys/a20os-dev.rsa --key-name a20os-dev.rsa.pub

# 只校验（src 是否都能匹配、元数据是否齐全），CI 用
tools/mka20pkg.py packages/recipes/a20-base.toml --arch riscv64 --check

# 不带签名（消费侧需 --allow-untrusted）
tools/mka20pkg.py packages/recipes/a20-base.toml --arch riscv64
```

make 集成（`tools/targets-pkg.mk`）会自动传入正确的 `--arch/--variant/
--kernel-build-dir/--sign-key`，日常使用 `make pkgs` 即可。

## 新增一个包的流程

1. 先让东西能被**构建**出来（user/Makefile 或 extra.mk，照旧）；
2. 新建 `packages/recipes/<name>.toml`：写元数据 + `[[files]]` 规则；
3. `make pkgs-check PKG_RECIPES=<name>` 校验；
4. `make pkgs PKG_RECIPES=<name>` 打包，`tar -tzf` 检查内容；
5. 加进某个 world（`packages/world/*.world`）让它进入镜像。

## 版本与 release 号约定

- `version` 跟随内容来源：移植 vim 9.2 就是 `"9.2.x"`，项目自身组件用项目版本；
- 内容没变、只是打包方式/依赖修正时，递增 `release`（apk 以
  `version-r{release}` 整体比较新旧）；
- 不要降级 version 来"回滚"——apk 不会把更小的版本号当更新。

## 常见问题

**recipe 报错 "src pattern matched nothing"** —— 产物还没构建。
a20-base/a20-drivers 需要 `make dev-build`；a20-kernel 需要内核构建；
a20-extra-* 需要 `make extra-user-apps EXTRA_PACKAGES=...`。想让规则
"有就装没有就算"，加 `optional = true`。

**想要排除某个文件** —— `exclude` 按文件名匹配（不含路径），如
`exclude = ["sh", "*.o"]`。

**包之间的文件冲突** —— apk 拒绝两个包装同一路径（后者报错）。
典型例子：构建目录里已有 `sh` 文件时，不要再 `symlink` 一个 `/sh`
（a20-base 的处理方式是 exclude 掉构建产物里的 sh，统一用链接）。
