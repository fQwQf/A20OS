# 内核扩展点（KEP）

A20OS 的动态内核扩展机制。与 Linux LKM（可加载内核模块）有本质区别：

| | Linux LKM | A20OS KEP |
|---|---|---|
| 扩展形态 | 任意原生代码（编译进内核空间） | 验证过的受限字节码程序 |
| 权限 | 加载即全权（可访问任何内核符号） | 只能读/写扩展点发布的类型化上下文 |
| 安全 | 模块崩溃 = 内核崩溃；任意指针访问 | 无指针、无任意内存访问、无循环（结构上终止） |
| 生命周期 | insmod/rmmod，ABI 漂移风险 | 程序对象引用计数，进程退出自动清理 |
| 卸载 | 需要模块作者处理引用 | detach/release 由内核管理 |

## 设计原则

- **混合内核的扩展观**：热路径留在内核，但扩展不能成为"第二个内核"。
  KEP 程序是数据面逻辑（过滤、审计、策略判定），运行在内核扩展点，
  由线性扫描验证器保证不可破坏内核状态。
- **扩展点 = 类型化上下文**：内核子系统注册扩展点并声明上下文布局
  （64 位字数组）。程序只能通过 `LDC`/`STC` 访问该窗口。
- **终止性 = 结构性保证**：所有跳转严格向前（imm 无符号），验证器
  线性扫描确认，解释器另有指令预算兜底。

## 指令集（32 位定长）

`op<<28 | rd<<24 | rs<<20 | aux<<16 | imm16`

| op | 助记符 | 语义 |
|---|---|---|
| 0 | MOVI rd, imm16 | 立即数（符号扩展） |
| 1 | MOV rd, rs | 寄存器拷贝 |
| 2 | LDC rd, off16 | 从上下文窗口读一个字 |
| 3 | STC off16, rs | 写上下文窗口 |
| 4 | ALU rd, rs, aux | add/sub/and/or/xor/shl/shr/neg |
| 5-10 | ADDI/ANDI/ORI/XORI/SHLI/SHRI | 立即数算术 |
| 11 | JMP off16 | 相对跳转（仅向前） |
| 12 | JCC rd, rs, cc, off16 | 条件跳转（EQ..GE，aux bit3 有符号） |
| 13 | EXIT | 返回 R0（作为判定值） |

8 个 64 位寄存器（R0..R7），R0 是返回值。程序 ≤ 256 条指令，
上下文 ≤ 64 字。

## 验证器

线性扫描逐条检查：寄存器/助记符字段界内；`LDC`/`STC` 偏移在
`KEP_MAX_CONTEXT_WORDS` 内；跳转目标在程序内且严格向前；程序以 EXIT
结尾。任何非法指令或越界访问都导致加载拒绝（-EINVAL）。

## 扩展点：syscall 过滤器

第一个扩展点（id=1，`syscall-filter`），上下文 8 字：

```
0 nr  1-6 arg0-5  7 abi (0=Linux, 1=Native)
```

附着程序在每次系统调用入口执行（两个 ABI 都生效），R0 判定：
`0` 放行、`1` 拒绝（-EACCES / -A20_ERR_ACCESS）、`2` 终止调用者。

## 接口（Native ABI 0x0D00）

- `ext_prog_load(insns, len) → handle`：验证并加载程序（A20_OBJ_EXT_PROG）
- `ext_prog_attach(prog, point)` / `ext_prog_detach(prog, point)`
- `ext_prog_release(prog)`：分离并释放
- `ext_point_info(point, out)`：查询扩展点（名称、上下文大小）

程序所有权：加载进程持有；进程退出自动释放；handle 关闭（最后一个引用）
触发释放。权限：READ=attach/detach，CONTROL=release。

## 验证

`smoke-native-ext`：加载/非法程序拒绝/attach 后 syscall 被拒/
detach 恢复/release 全流程。
