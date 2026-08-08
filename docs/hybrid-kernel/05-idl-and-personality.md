# Native 服务 IDL 与 Linux 人格层

本文档定义 A20OS Native 服务的 **IDL 语言规范**、当前实现状态，以及 Linux 人格层的两阶段实现。设计定位见 [00-design.md](00-design.md)，改造路线见 [03-refactor-plan.md](03-refactor-plan.md) 阶段四/五。

## 1. IDL 语言规范

A20 服务 IDL 是一份很小的声明式语言，由 `tools/a20idl.py` 编译为确定性的 C 头文件。**当前只覆盖常量与固定宽度消息**：版本号、`const`、`message` 结构。消息布局与类型化绑定生成是下一扩展；保持语法小巧使协议版本化与生成源比对立即可用。

### 1.1 源文件语法

```
// 注释：// 开头整行忽略；空行忽略。
interface <名字> version <N> {
  message <名字> {
    <类型> <字段名>;
    ...
  }
  const <名字> = <值>;
  ...
}
```

逐行解析，规则如下：

| 规则 | 说明 |
|------|------|
| `interface <名字> version <N> {` | **必须出现在第一条有效行**，且只能出现一次；`<N>` 为十进制整数，生成 `A20_SERVICES_IDL_VERSION` |
| `message <名字> { ... }` | 生成 `typedef struct a20_idl_<名字小写>_t { ... }`；字段必须是固定宽度类型，每个字段一行以 `;` 结尾 |
| `const <名字> = <值>;` | 生成 `#define <名字> (<值>)`；`<值>` 是 `;` 前的原文（可含十六进制/十进制/表达式） |
| 字段类型 | 只能是 `u8 u16 u32 u64 i32 i64`，映射为 `uint8_t ... int64_t` |
| 非法行 | 未匹配任何规则的**非注释非空**行 → 生成器以非零退出 |

解析顺序：`version` 只在前置阶段读取；进入 `message` 块后只接受字段或 `}`；块外优先匹配 `message`，再匹配 `const`。

### 1.2 生成器约束（`tools/a20idl.py`）

- 源文件必须为 **ASCII** 编码；
- `version` 缺失 → 报错 `missing interface version`；
- `const` 名字重复 → 报错 `duplicate constant`；
- message 内非法字段行 → 报错 `malformed message field`；
- **不校验** 字段名字冲突、常量与宏展开后的表达式合法性、以及生成结构体的对齐/大小——调用方以 `_Static_assert` 或 wire 解析器保证布局一致；
- 输出固定 header：`A20_SERVICES_IDL_VERSION` 宏、常量宏（按出现顺序）、消息 `typedef struct`（按出现顺序）、include guard `_A20_SERVICES_IDL_H`，并 include `a20_types.h`。

### 1.3 wire 布局语义

- 固定宽度消息按字段顺序紧凑布局；**结构体对齐由编译目标决定**，IDL 不承诺无 padding（如需 packed 布局须在生成结构体上显式声明，当前生成器不添加 `__attribute__((packed))`）；
- 服务消息在 channel 上传输时携带 `{version, type, size}` 信封（见下文 1.4），接收端按 `version`/`size` 校验，避免错配或越界解析；
- `const` 值用于请求/响应 wire type 编号与错误码，语义由服务协议决定。

### 1.4 版本化请求/响应信封

所有服务消息使用 `ENVELOPE` 协议：

```
typedef struct a20_idl_envelope_t {   // message ENVELOPE
    uint16_t version;                 // 协议版本，接收端校验
    uint16_t type;                    // RTCD_REQ_* / RTCD_REPLY_* / SVCMGR_REQ_*
    uint32_t size;                    // payload 长度，接收端校验
} a20_idl_envelope_t;
```

- 请求与响应使用**独立 wire type**（如 `RTCD_REQ_TIME`=0x54 与 `RTCD_REPLY_TIME`=0x61），避免请求/响应误判；
- 服务端校验 `version` 与 `size`，`size` 超限或不匹配 → 丢弃并返回错误，**不越界解析**；
- 畸形消息（版本不符、类型未知、字段越界）被忽略而非导致服务崩溃——`echod` 即以此行为拒绝非法输入。

### 1.5 版本化规则

- 接口版本 `N` 增加表示 wire 语义变化；接收端用 `A20_SERVICES_IDL_VERSION` 与信封 `version` 比对；
- **当前**只做"严格相等校验 + 拒绝"：版本不匹配即拒绝该消息（不实现动态协商）；
- 协议演进（新增消息/字段）应先 bump 版本，再同步服务端与生成头，避免运行期错配。

### 1.6 扩展流程（新增一个服务协议）

1. 在 `user/svc/a20_services.idl` 增加 `const` 与 `message`，必要时 bump `version`；
2. 运行 `make check-a20-idl`（在 `a20os` conda 环境中重生成并比对活跃头，本机无 conda 时在配置好 `a20os` 的环境执行）；
3. 服务端（rtcd/svcmgr/ubd 等）与客户端按新生成头收发，字段布局由 `typedef struct a20_idl_*_t` 保证一致；
4. 服务端信封校验保持 version/size 检查，防止旧客户端错配。

### 1.7 当前 IDL 覆盖与边界

- **已覆盖**：`a20_services.idl` version 1 定义 `ENVELOPE`、`RTCD_ALARM_REQUEST`、`RTCD_TIME_RESPONSE`、`SVCMGR_ECHO` 四个消息，以及 RTCD/UBD/SVCMGR 的请求/响应/崩溃常量；
- **不在 IDL 范围**：registry 是内核 syscall 接口（`0x0A03`），不属于 channel 协议；
- **剩余**：双端绑定代码生成与动态版本协商（生成器当前只产 C 头，不产收发 stubs）。

## 2. 实现状态（与 `make check-a20-idl` 对应）

服务协议已从手写 proto 头迁移到 `user/svc/a20_services.idl`。`tools/a20idl.py` 生成 `user/svc/a20_services_idl.h`，rtcd、svcmgr 和 ubd 的协议头只保留槽位/设备常量并 include 生成头。

已实现：

- 版本化常量与固定宽度 `message` 字段（生成结构体，wire layout 保持）；
- 版本化请求/响应信封（见 1.4）；`smoke-native-rtcd` 双向验证；
- svcmgr/echod 协议 IDL 化（`SVCMGR_REQ_ECHO/CRASH` 版本化消息替换裸 echo 与魔法字符串），echod 校验版本并忽略畸形消息而非误崩溃；`smoke-native-svc` PASS；
- `make check-a20-idl` 重生成并比较活跃头。

## 3. Linux 人格层

在 Native 原语上重建的 Linux 风格接口，两阶段已实现：

1. `a20_personality.h` — channel-backed pipe facade：跨消息字节流（NONBLOCK drain 拼接）、部分读取保留剩余、level 触发的就绪（pending 数据保持可读直到耗尽）；
2. `a20_linux.h` — 对象翻译层：fd 表（open/close/dup/read/write，`A20_LINUX_FD_MAX=64`）、匿名 mmap、pipe、socketpair、futex（ETIMEDOUT 映射）、epoll 风格 wait-many（共享 EventQ）；`smoke-native-linux` 六分区 PASS。

**语义对照**：`user/cmds/core/pipe_ref.c` 用真实 Linux pipe(2) 执行与 native 实现相同的序列（部分读、跨消息字节流、level 就绪），`smoke-native-personality` 要求两个实现输出完全一致的 `PIPE_REF` 行——native 与 Linux ABI 语义对照成立。

剩余：fd 表 byte-stream 语义的完整覆盖、epoll level 触发通用化、更大测例集（CAgent 功能项）的语义 diff 与性能对照。本文档不把现有 Linux ABI 直通实现称为已完成的 starnix 人格层；Native ABI 仍是系统本体，完整人格负载是后续工作。
