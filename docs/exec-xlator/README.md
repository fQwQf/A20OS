# exec-xlator：外来架构二进制在 execve 上透明转发

`execve` 一个外来架构（x86_64 / aarch64）的 Linux 可执行文件，内核把它改写成一次对用户态翻译器的就地 re-exec，guest 因此作为普通 Linux-ABI 进程运行，对调度器、`/proc`、fd 表、stdio、退出码与原生进程无异。

默认全部关闭：不传启动键时，`execve` 外来二进制返回 `Exec format error`，与接入这个特性之前逐字节一致。

## 三分钟跑通

在 riscv64 QEMU 上，把 x86_64 guest 交给 qemu-user：

```
# 1. 构建：把宿主原生的 qemu-x86_64 与交叉编译的 x86_64 探针拉进镜像
make ARCH=riscv64 ABI=linux XLATOR=1 XLATOR_GUEST=x86_64 dev-build

# 2. 启动，命令行给出开关与翻译器路径
make ARCH=riscv64 ABI=linux run \
  QEMU_FLAGS_EXTRA="" EXTRA_QEMU="-append 'a20.xlator=1 a20.xlator.x86_64=/bin/qemu-x86_64'"

# 3. 在 guest shell 里验证
/bin/xlate_exec foreign /bin/xlate_probe-x86_64      # 期望：退出码 0，输出 XLATE_PROBE: MARK=...
cat /proc/a20/xlator                                  # 期望：enabled: 1，且该 guest 有 path
```

没有交叉编译器、也不想下载任何东西的话，用仓内自带的 shim 走一遍形状验证，见 [01-usage.md](01-usage.md#7-不用交叉编译器的验证路径) 或直接 `make smoke-exec-xlator-shim`。

## 按你的角色选入口

| 你是 | 读 | 回答的问题 |
|---|---|---|
| 用这套功能的人（配置、排查） | [01-usage.md](01-usage.md) | 有哪些启动键、模板怎么写、运行期怎么开关、出错了怎么看 |
| 要接入自己翻译器的人 | [02-integration.md](02-integration.md) | 我的翻译器需要满足什么、要改几处、怎么验证、AOT 行不行 |
| 要改这段内核代码的人 | [03-internals.md](03-internals.md) | 为什么是这个形态、代码在哪、内核 API 契约、不变量、已知代价 |
| 要扩展这条通道形状的人 | [04-extending.md](04-extending.md) | 三条轴怎么分、加 native-ABI 翻译器要改什么、AOT 落在哪一步、binfmt_misc 借鉴什么 |

四篇的分工是硬边界：**能靠配置表达的，不写进代码；只属于设计取舍的，不混进使用手册。** 参数表只在 01 出现一次，02 只引用不复述。04 与前三篇的重叠只有一处（注册表四列的格式），且 04 是唯一解释「为什么第三列存在」的地方。

## 覆盖范围与不覆盖范围

覆盖：

- 静态、64 位、小端的**Linux ABI** 可执行文件，guest 用 qemu-user 一类的翻译器运行。
- 多个 guest 架构同时配置，各用各的调用约定。
- 按 `(架构, ABI)` 二元组匹配：A20OS 的两种 ABI 对同一台机器是同一个 ELF64，只有 `PT_A20_START_INFO` 段能区分。

**不**覆盖（明确的已知边界，不是待办）：

- **A20 native ABI 的 guest。** 键已经就位（`elf_guest_key_t` 带 ABI），注册表里没有 `abi=native` 的行，因为还没有 native-ABI 翻译器——qemu-user 只实现 Linux ABI。这类镜像现在返回 `ENOEXEC`。要加的话改哪些地方见 [04-extending.md](04-extending.md#3-加一个-native-abi-翻译器需要改什么)。
- 外来架构**内核**或完整发行版——那是虚拟化，见 [../hybrid-kernel/00-design.md](../hybrid-kernel/00-design.md)。
- Android APK：没有 bionic/ART/dex 运行时。
- 动态链接 guest 的 sysroot 与 loader 解析（`-L` / `QEMU_LD_PREFIX` 留作后续）。
- 提前编译型（AOT）翻译器：需要新机制，不是配置项，理由见 [02-integration.md](02-integration.md#5-aot-型翻译器prism-一类尚不支持)，接口落点见 [04-extending.md](04-extending.md#5-编译器型aot翻译器需要的那一整块)。
- 任何性能宣称。全部数据来自 QEMU TCG 之上再叠一层仿真。

## 相关入口

- 运行验证与门禁：[../testing-gates.md](../testing-gates.md)（`smoke-exec-xlator`、`smoke-exec-xlator-shim`、`smoke-exec-xlator-off`、`smoke-exec-xlator-la64`、`check-xlator-guests`）
- 能力表条目：[../hybrid-kernel/STATUS.md](../hybrid-kernel/STATUS.md)
- 与 Linux 人格层（阶段五）的分工：[../hybrid-kernel/05-idl-and-personality.md](../hybrid-kernel/05-idl-and-personality.md)
- 代码位置速查：[03-internals.md](03-internals.md#1-代码地图)