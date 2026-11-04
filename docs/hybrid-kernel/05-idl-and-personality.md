# Native 服务 IDL 与 Linux 人格层

本文档规范 Native 微内核服务接口定义语言（IDL）、通信桩代码生成与 Linux 人格层映射协议。

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
| `interface <名字> version <N> {` | 必须出现在第一条有效行，且只能出现一次；`<N>` 为十进制整数，生成 `A20_SERVICES_IDL_VERSION` |
| `message <名字> { ... }` | 生成 `typedef struct a20_idl_<名字小写>_t { ... }`；字段必须是固定宽度类型，每个字段一行以 `;` 结尾 |
| `const <名字> = <值>;` | 生成 `#define <名字> (<值>)`；`<值>` 是 `;` 前的原文（可含十六进制/十进制/表达式） |
| 字段类型 | 只能是 `u8 u16 u32 u64 i32 i64`，映射为 `uint8_t ... int64_t` |
| 非法行 | 未匹配任何规则的非注释非空行 → 生成器以非零退出 |

解析顺序：`version` 只在前置阶段读取；进入 `message` 块后只接受字段或 `}`；块外优先匹配 `message`，再匹配 `const`。

### 1.2 生成器约束（`tools/a20idl.py`）

- 源文件必须为 ASCII 编码；
- `version` 缺失 → 报错 `missing interface version`；
- `const` 名字重复 → 报错 `duplicate constant`；
- message 内非法字段行 → 报错 `malformed message field`；
- 空 message → 报错 `empty message`（否则会生成非法的空结构体，以及一对没有字段的 codec）；
- message 块未闭合 → 报错 `unterminated message: <名字>`（此前会被静默丢弃，等于无声地从 wire 契约里删掉一条消息）；
- 同一 message 内字段名重复 → 报错 `duplicate field: <字段> in message <消息>`；仅大小写不同也算重复（offset 宏会被大写，撞名时第二个定义胜出，两个字段写到同一偏移，非紧凑消息甚至编译通过也不报错）；
- message 名重复 → 报错 `duplicate message: <名字>`（typedef、`A20_IDL_<名字>_*` 宏与 encode/decode 三处都会撞）；
- 不校验常量与宏展开后的表达式合法性；
- 每个 message 除 `typedef struct` 外还生成 wire 契约与编解码 helpers（见 1.3）；
- 输出固定 header：`A20_SERVICES_IDL_VERSION` 宏、常量宏（按出现顺序）、目标字节序限制、标量读写 helpers、消息 `typedef struct` + `A20_IDL_*_WIRE_SIZE` / `A20_IDL_*_OFF_*` + （仅紧凑消息）`_Static_assert` + `a20_idl_<名字>_encode/decode`（按出现顺序）、include guard `_A20_SERVICES_IDL_H`，并 include `a20_types.h` 与 `<stddef.h>`（`offsetof`）。

### 1.3 wire 布局语义

**编码格式与结构体内存布局具有明确区分：**

*codec（byte-wise）就是 wire 格式*：字段为固定宽度整数、小端编码（有符号类型为二补码），按声明顺序紧凑排列、无 padding，消息长度等于各字段宽度之和。生成头把这些数字写成 `A20_IDL_<消息>_WIRE_SIZE` 与 `A20_IDL_<消息>_OFF_<字段>`，`a20_idl_<消息>_encode/decode` 严格按这些偏移逐字节读写。标量 helpers 是 shift-or 而不是 memcpy，字节序是写下来的，不是从目标继承的。

*生成结构体是宿主 C 布局，不是 wire 格式*：`sizeof`/`offsetof` 只说明每个字段从哪里开始、对象多大，**对字节序只字未提**。大端宿主的同一份结构体，偏移与尺寸完全相同，字节完全相反。

由此得到两条结论：

1. **紧凑消息**（字段宽度使宿主结构体恰好无空洞——v1 的四条消息都属于此类）：生成器对 `sizeof` 与每个 `offsetof` 逐条 `_Static_assert`。这让“把结构体内存直接交给 channel”成为**被校验过的**性质，而不是假设：若某目标的 ABI 会插入 padding，构建直接失败，不会静默改变 v1 字节。但这些 assert 只钉住布局，**不钉字节序**。
2. **非紧凑消息**（如 `{u8,u16,u32,u64}`：wire 15 字节、偏移 0/1/3/7，而宿主结构体 16 字节且 `count` 在偏移 4）：生成器**不发出**布局 assert，因为那会让 header 根本编译不过。此时结构体不是 wire 布局，必须用 codec 或一个 `A20_IDL_<消息>_WIRE_SIZE` 大小的缓冲区产生字节。

**目标字节序限制**：version 1 的字节序是 wire 的属性，不是目标的属性。因为上面第 1 条的 assert 回答不了字节序，生成头单独做一次检查——`__BYTE_ORDER__` 不是 `__ORDER_LITTLE_ENDIAN__` 则 `#error`；工具链未定义 `__BYTE_ORDER__` 同样 `#error`（fail closed，不做假设）。检查通过后定义 `A20_SERVICES_IDL_TARGET_IS_LITTLE_ENDIAN`，供需要显式依赖“宿主字节序 == wire 字节序”的程序判断。也就是说：大端目标上的 codec 本身仍然正确，但凡是把结构体内存直接上线的地方都不再被允许存在。

- 服务消息在 channel 上传输时携带 `{version, type, size}` 信封（见下文 1.4），接收端按 `version`/`size` 校验，避免错配或越界解析；
- `const` 值用于请求/响应 wire type 编号与错误码，语义由服务协议决定。

### 1.4 版本化请求/响应信封

rtcd 与 svcmgr/echod 的 channel 消息使用 `ENVELOPE` 协议：

```
typedef struct a20_idl_envelope_t {   // message ENVELOPE
    uint16_t version;                 // 协议版本，接收端校验
    uint16_t type;                    // RTCD_REQ_* / RTCD_REPLY_* / SVCMGR_REQ_*
    uint32_t size;                    // 整条消息长度（含信封本身），接收端校验
} a20_idl_envelope_t;
```

- 请求与响应使用独立 wire type（如 `RTCD_REQ_TIME`=0x54 与 `RTCD_REPLY_TIME`=0x61），避免请求/响应误判；
- 服务端校验 `version` 与 `size`，`size` 超限或不匹配 → 丢弃并返回错误，不越界解析；
- 畸形消息（版本不符、类型未知、字段越界）被忽略而非导致服务崩溃，`echod` 即以此行为拒绝非法输入。

`size` 统计的是**整条消息**而不是 payload：`echod` 拿它和 channel 收到的 `blen` 直接比较，`svcman` 发请求时填的也是 `sizeof(整帧)`。这是一个取值约定而非布局问题，因此不进入生成器，只留在构造/校验它的调用点（`user/svc/svcman.c` / `user/svc/echod.c`）；生成器只保证 `size` 字段本身位于偏移 4、按小端 u32 编码。

### 1.5 版本化规则

- 接口版本 `N` 增加表示 wire 语义变化；接收端用 `A20_SERVICES_IDL_VERSION` 与信封 `version` 比对；
- 数据面（echod/rtcd/ufsd）只做严格相等校验：`version != 1` 或 `size != 收到长度` 即丢弃，不解析负载、不回复；
- `SVCMGR_VERSION_NEGOTIATE`/`REP_VERSION_NEGOTIATE` 是控制面的动态协商：客户端发送自己支持的`[min,max]` 区间，服务端在交集里选取并回 ACK；没有交集时回 `NO_COMMON`，区间非法时回`INVALID_RANGE`。ACK 必须精确匹配 nonce、类型、信封版本、长度和状态；任何不一致（包括未知 status、未知 reply type、尾随字节、legacy echo 不覆盖 v1）都拒绝，绝不回退到 legacy；
- legacy 回退只在服务端把请求原样回显、且客户端自己也支持 v1 时成立；
- 各消息族的未知值契约是明确的，不靠相邻分支推断：

| 消息族 | 未知 version | 未知 type | 未知 status/opcode |
| --- | --- | --- | --- |
| 控制面协商 ACK | 拒绝 | 拒绝 | 拒绝（`InvalidStatus`/类型不符），不回退 |
| SVCMGR (`echod`) | 丢弃、不回复 | 原样回显 | — |
| RTCD | 丢弃、不回复 | 消费但不回复 | — |
| UFSD 二进制协议 | 丢弃、不回复 | 回复 `-ENOSYS` | 回复 `-ENOSYS` |
| UFSD 新协议信封 | 丢弃、不回复 | 按 opcode 分派；未知 opcode `-ENOSYS` | — |

- 协议演进（新增消息/字段）应先 bump 版本，再同步服务端与生成头，避免运行期错配。

### 1.6 扩展流程（新增一个服务协议）

1. 在 `user/svc/a20_services.idl` 增加 `const` 与 `message`，必要时 bump `version`；
2. 服务端（rtcd/svcmgr/ubd 等）与客户端按新生成头收发，字段布局由 `typedef struct a20_idl_*_t` 与生成头里的 `_Static_assert` 共同保证一致；
3. 服务端信封校验保持 version/size 检查，防止旧客户端错配。

生成头 `user/svc/a20_services_idl.h` 是构建期自动生成的（`tools/targets-native.mk` 的 `$(A20_SERVICES_IDL_HDR)` 规则，`A20_IDL_PYTHON` 可覆盖，默认 `python3`），并被 `.gitignore` 排除、不进入 Git 跟踪；native 服务二进制直接依赖该头并自动触发生成：绑定槽位（svc/ping/shmring/chand/rtcd/ubd）、shmring 几何、消息布局与 wire 编解码 helpers 均出自 IDL。`make check-a20-idl` 重新生成并与当前工作树头比对，防止手工改动生成头。

### 1.7 当前 IDL 覆盖与边界

- 已覆盖：`a20_services.idl` version 1 定义 `ENVELOPE`、`RTCD_ALARM_REQUEST`、`RTCD_TIME_RESPONSE`、`SVCMGR_ECHO` 四个消息，以及 RTCD/UBD/SVCMGR 常量；rtcd 与 svcmgr/echod 实际使用 envelope，ubd 只消费生成常量且块数据面走共享环；
- 不在 IDL 范围：registry 是内核 syscall 接口（`0x0A03`），不属于 channel 协议；
- 剩余：双端绑定代码生成与动态版本协商（生成器当前只产 C 头，不产收发 stubs）。

## 2. 实现状态（与 `make check-a20-idl` 对应）

常量、消息布局与服务绑定槽位均已迁移到 `user/svc/a20_services.idl`。`tools/a20idl.py` 在构建期生成 ignored 的 `user/svc/a20_services_idl.h`；原 `rtcd_proto.h`、`svc_proto.h`、`ubd_proto.h` 与 `shmring_proto.h` 手写 wrapper 已删除，退出源码树。

已实现：

- 版本化常量与固定宽度 `message` 字段（生成结构体，wire layout 保持）；
- 显式小端 wire 编解码：生成头给出每条消息的打包长度与字段偏移、目标字节序限制、仅对紧凑消息的 `_Static_assert`，并提供 `a20_idl_*_encode/decode`；`echod`/`svcman` 的收发字节改由这些 helpers 产生与解析，v1 字节不变；
- 三条 host 侧回归：
  - `tools/tests/test_a20idl.py`——生成器输出、wire 尺寸/偏移、拒绝路径，以及**用编译器验证**字节序限制（以 `-D__BYTE_ORDER__=__ORDER_BIG_ENDIAN__` 编译真实生成头，必须失败；无编译器时跳过），经 `make check-a20-tests` 运行；
  - `tools/tests/test_a20_idl_wire.c`——v1 真实字节、codec 与旧结构体写法逐字节等价、echod 的长度/版本/size 忽略规则；
  - `tools/tests/test_a20_idl_mixed.c` + `tools/tests/idl_mixed.idl`——**编译并运行**生成器对一条非紧凑 `{u8,u16,u32,u64}` 消息的输出，核对 encode/decode 的小端 golden 字节。后两条经 `make host-tests` 运行。字符串断言无法发现“生成出合法外观但编译不过的 C”（u8 字段曾经生成 `buf + OFF = src->f;`），只有编译过的测试能。
- 版本化请求/响应信封（见 1.4）；`smoke-native-rtcd` 双向验证；
- svcmgr/echod 协议 IDL 化（`SVCMGR_REQ_ECHO/CRASH` 版本化消息替换裸 echo 与魔法字符串），echod 校验版本并忽略畸形消息而非误崩溃；`smoke-native-svc` 是对应运行入口，本次未运行；
- `make check-a20-idl` 重生成并与当前工作树生成头比较。

## 3. Linux 人格层

在 Native 原语上重建的 Linux 风格接口，两阶段已实现：

1. `a20_personality.h` — channel-backed pipe facade：跨消息字节流（NONBLOCK drain 拼接）、部分读取保留剩余、level 触发的就绪（pending 数据保持可读直到耗尽）；
2. `a20_linux.h` — 对象翻译层：fd 表（open/close/dup/read/write，`A20_LINUX_FD_MAX=64`）、匿名 mmap、pipe、socketpair、futex（ETIMEDOUT 映射）、epoll 风格 wait-many（共享 EventQ）；`test_native_linux.c` 有六个分区，`smoke-native-linux` 是对应运行入口，本次未运行。

语义对照：`user/cmds/core/pipe_ref.c` 用真实 Linux pipe(2) 执行与 native 实现相同的序列（部分读、跨消息字节流、level 就绪），`smoke-native-personality` 要求两个实现输出完全一致的 `PIPE_REF` 行。源码具备该对照入口；本次未运行，不能把目标断言写成当前 PASS。

剩余：fd 表 byte-stream 语义的完整覆盖、epoll level 触发通用化、更大测例集（Linux 功能测例）的语义 diff 与性能对照。现有 Linux ABI 直通实现不等于已完成的 starnix 人格层；Native ABI 仍是系统本体，完整人格负载是后续工作。
