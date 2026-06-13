# Native ABI 运行时状态

本文档记录 A20OS Native ABI 当前用户态运行时的真实状态、已知偏差和后续路线图。它是对 [00-overview.md](00-overview.md) 中实现状态的补充。

## 当前运行时状态

| 组件 | 路径 | 状态 | 说明 |
|------|------|------|------|
| Linux ABI 兼容层 | `kernel/abi/linux/` | 活跃 | 当前主用户态运行时接口。系统启动后实际运行的用户态程序基于 Linux ABI。 |
| Native ABI 内核入口 | `kernel/abi/native/` | 已实现 | 90 个 syscall 入口已完成，覆盖 core、handle、task、memory、path、ipc、net、time、security、debug、system info 等分区。 |
| liba20rt Native SDK | `user/liba20rt/` | 活跃 | 当前活跃的原生 SDK，提供 `a20_syscall.h` syscall wrapper、多架构 crt0 启动汇编、`a20_types.h` ABI 类型定义和高层 handle I/O 头文件。 |
| liba20c 最小 C 库 | `user/liba20c/` | 活跃 | 已实现 malloc、stdio、unistd、string、time、errno、fdtable 等 ISO C 子集。`malloc.c`、`unistd.c`、`stdio.c`、`bare_alloc.c` 已改为使用带 `size` 和 `version` 的版本化 ABI 结构体调用 syscall。 |
| 原生测试 | `user/tests/test_native_handle.c`、`user/liba20c` 内部测试 | 少量存在 | 当前仅有少量示例测试，不是历史上宣称的 4 套 118 cases host-mode 测试套件。 |
| musl 移植目录 | `user/musl-port/` | 不存在 | 该目录未在当前仓库中创建。相关历史材料已移至 `user/archive/`。 |
| 历史参考 | `user/archive/` | 不参与构建 | 包含旧版 musl 桥接、`a20coreutils`、`build_sysroot.sh`、`arch/a20/` 适配头等。这些代码仅供历史参考，路径和内容已过时，不进入当前构建。 |
| Debug handle | `kernel/abi/native/` 0x0900 分区 | 有意受限 | 仅支持创建 debug handle、读取寄存器快照和映射目标内存。stop/resume/watchpoint、ptrace 级语义等尚未实现，也不会在需求明确前扩展。 |

## 关键偏差说明

### 1. liba20c 已迁移到版本化 ABI 结构体

`liba20c` 的 `malloc.c`、`unistd.c`、`stdio.c`、`bare_alloc.c` 已完成迁移：不再使用裸 `uint64_t args[N]` 数组，而是构造带 `.size` 和 `.version` 的版本化结构体（如 `a20_vm_alloc_args_t`、`a20_path_open_args_t`、`a20_io_args_t`）后调用 syscall。

示例（迁移后）：

```c
struct a20_vm_alloc_args args = {
    .size = sizeof(args),
    .version = 1,
    .length = req_size,
    .prot = A20_PROT_READ | A20_PROT_WRITE,
};
int64_t r = a20_vm_alloc(&args);
```

迁移后，`smoke-native-libc` 目标已跑通，裸参数数组调用已不存在于 `user/liba20c/*.c`。

### 2. liba20rt 类型头与内核类型头存在布局偏差

`user/liba20rt/a20_types.h` 和 `kernel/include/abi/native/types.h` 在部分结构体字段上存在不一致。虽然 `a20_types.h` 的注释声明“MUST match kernel/include/abi/native/types.h exactly”，但实际代码尚未完全对齐。任何使用这些结构体的新代码都应以内核头为准，并在修复时同步更新 `liba20rt`。

### 3. archive 目录不参与构建

`user/archive/build_sysroot.sh` 引用了 `user/musl-port/`、`user/archive/src/...` 等路径，其中一些在当前仓库中已不存在。不要直接运行该脚本。如果未来需要重新启动完整 musl 移植，应以 `user/archive/` 为参考，而不是直接复用。

### 4. 测试覆盖有限

当前原生测试只有 `user/tests/test_native_handle.c` 和 `user/liba20c` 内部的小规模示例。历史上文档中提到的 4 套 118 cases host-mode 测试套件目前不存在，需要在路线图阶段补齐。

## 路线图

### 近期：修复 liba20c ABI 结构体使用（已完成）

- [x] 将 `liba20c` 中所有裸参数数组调用改为带 `size` 和 `version` 的版本化 ABI 结构体。
- [x] 统一使用 `a20_types.h` 中定义的 `a20_vm_alloc_args_t`、`a20_path_open_args_t`、`a20_io_args_t` 等结构体。
- [x] 在修改过程中对照 `kernel/include/abi/native/types.h`，同步修正 `liba20rt/a20_types.h` 中的布局偏差。
- [x] 完成后在目标架构上跑通现有 liba20c 示例测试（`smoke-native-libc`）。

### 中期：补齐原生测试与示例

- 为 `liba20rt` 添加 handle I/O、channel、event queue 的基础测试。
- 为 `liba20c` 添加 malloc、stdio、unistd 的回归测试。
- 建立 `smoke-native-libc` 或类似的最小 smoke 目标。

### 远期：按需扩展 Native 用户态生态

- 明确需求后，决定是否重新启动完整 musl 移植。
- 若重启，应基于 `user/archive/` 的参考代码重新设计，而不是直接复用旧路径。
- Debug handle 的 stop/resume/watchpoint 能力也只在有明确调试需求时扩展。

## 与用户决策的对应关系

- 用户已确认：Linux ABI 继续作为主用户态接口，`abi/native` 保持为辅。
- 用户已确认：`liba20c` 将更新为使用版本化 ABI 结构体。该代码变更不在本文档更新范围内，仅作为路线图记录。
- 用户已确认：`user/archive/` 作为历史参考保留，不参与当前构建。
- 用户已确认：Debug handle 保持 intentionally limited，不盲目扩展为完整 ptrace。
