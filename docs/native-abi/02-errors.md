# A20OS Native ABI：错误码与返回约定

> 本文档定义 A20OS Native ABI 的错误码、返回值约定和错误处理策略。

---

## 1. 返回值约定

所有 native syscall 返回 `a20_status_t`（`int64_t`）：

```text
>= 0  成功。具体含义视 syscall 而定：
      - 大部分操作返回 0 表示成功
      - 某些操作返回非负值（如 handle 编号，通过 out 字段返回）
<  0  错误。返回值为 -A20_ERR_* 的负数形式
```

Native ABI 的错误码不要求等于 Linux errno。兼容层可以在 libc 或 shim 中映射到 POSIX errno。

---

## 2. 错误码定义

```c
#define A20_OK                       0    /* 成功 */
#define A20_ERR_PERM                 1    /* 权限不足（非 handle rights，而是 UID/GID 等） */
#define A20_ERR_NO_ENTRY             2    /* 路径组件不存在 */
#define A20_ERR_INTERRUPTED          3    /* 操作被信号中断 */
#define A20_ERR_IO                   4    /* I/O 错误 */
#define A20_ERR_BAD_HANDLE           5    /* handle 无效或已关闭 */
#define A20_ERR_NO_MEMORY            6    /* 内存不足 */
#define A20_ERR_ACCESS               7    /* handle rights 不足 */
#define A20_ERR_FAULT                8    /* 用户指针无效 */
#define A20_ERR_BUSY                 9    /* 资源忙（如 channel 满） */
#define A20_ERR_EXISTS               10   /* 文件已存在 */
#define A20_ERR_NOT_SUPPORTED        11   /* 操作不支持 */
#define A20_ERR_INVALID_ARGUMENT     12   /* 参数无效（含保留位非零） */
#define A20_ERR_NO_SPACE             13   /* handle table 满或磁盘满 */
#define A20_ERR_NOT_DIR              14   /* 路径中非目录组件 */
#define A20_ERR_IS_DIR               15   /* 期望文件但得到目录 */
#define A20_ERR_NOT_EMPTY            16   /* 目录非空 */
#define A20_ERR_NAME_TOO_LONG        17   /* 文件名过长 */
#define A20_ERR_WOULD_BLOCK          18   /* 非阻塞操作无法立即完成 */
#define A20_ERR_TIMED_OUT            19   /* 操作超时 */
#define A20_ERR_CANCELED             20   /* 操作被取消 */
#define A20_ERR_PROTOCOL             21   /* 协议错误 */
#define A20_ERR_RANGE                22   /* 参数超出范围 */
```

---

## 3. 错误码分类

### 3.1 权限类

| 错误码 | 触发场景 |
|--------|---------|
| `A20_ERR_PERM` | UID/GID 权限不足（如 kill 其他用户的进程） |
| `A20_ERR_ACCESS` | handle rights 不足（如对只读 handle 执行 write） |

两者区分：`PERM` 是传统 POSIX 权限问题，`ACCESS` 是 capability rights 问题。Native ABI 中大部分权限检查使用 `ACCESS`。

### 3.2 Handle 类

| 错误码 | 触发场景 |
|--------|---------|
| `A20_ERR_BAD_HANDLE` | handle 编号超出范围、已关闭、或 entry 为空 |
| `A20_ERR_NO_SPACE` | handle table 达到上限（65536） |

### 3.3 参数类

| 错误码 | 触发场景 |
|--------|---------|
| `A20_ERR_INVALID_ARGUMENT` | 参数值非法、flag 保留位非零、结构体 version 未知 |
| `A20_ERR_FAULT` | 用户态指针指向无效地址 |
| `A20_ERR_RANGE` | offset/size 超出合法范围 |

### 3.4 资源类

| 错误码 | 触发场景 |
|--------|---------|
| `A20_ERR_NO_MEMORY` | 内核内存分配失败 |
| `A20_ERR_NO_ENTRY` | 文件/目录不存在 |
| `A20_ERR_EXISTS` | 创建文件但已存在 |
| `A20_ERR_BUSY` | channel 满、锁竞争等 |

### 3.5 I/O 类

| 错误码 | 触发场景 |
|--------|---------|
| `A20_ERR_IO` | 磁盘/设备 I/O 错误 |
| `A20_ERR_INTERRUPTED` | 阻塞操作被中断 |
| `A20_ERR_WOULD_BLOCK` | 非阻塞模式下的 EAGAIN 等价 |
| `A20_ERR_TIMED_OUT` | 超时到期 |
| `A20_ERR_CANCELED` | 操作被显式取消 |

### 3.6 协议类

| 错误码 | 触发场景 |
|--------|---------|
| `A20_ERR_PROTOCOL` | 网络协议错误、channel 协议违反 |
| `A20_ERR_NOT_SUPPORTED` | 操作不被当前对象类型支持 |

---

## 4. 错误处理策略

### 4.1 原子性保证

如果 syscall 返回错误，内核状态必须等同于该 syscall 从未发生。这是 native ABI 的基本不变式。

例外：`A20_ERR_INTERRUPTED` 和 `A20_ERR_TIMED_OUT` 可能表示操作部分完成。此时应通过 `out_count` 等输出字段判断实际完成的操作量。

### 4.2 与 POSIX errno 的映射

兼容层（POSIX shim）需要将 native 错误码映射到 POSIX errno：

```text
A20_OK                →  0 (no error)
A20_ERR_PERM          →  EPERM
A20_ERR_NO_ENTRY      →  ENOENT
A20_ERR_INTERRUPTED   →  EINTR
A20_ERR_IO            →  EIO
A20_ERR_BAD_HANDLE    →  EBADF
A20_ERR_NO_MEMORY     →  ENOMEM
A20_ERR_ACCESS        →  EACCES
A20_ERR_FAULT         →  EFAULT
A20_ERR_BUSY          →  EBUSY
A20_ERR_EXISTS        →  EEXIST
A20_ERR_NOT_SUPPORTED →  ENOSYS
A20_ERR_INVALID_ARGUMENT → EINVAL
A20_ERR_NO_SPACE      →  ENOSPC
A20_ERR_NOT_DIR       →  ENOTDIR
A20_ERR_IS_DIR        →  EISDIR
A20_ERR_NOT_EMPTY     →  ENOTEMPTY
A20_ERR_NAME_TOO_LONG →  ENAMETOOLONG
A20_ERR_WOULD_BLOCK   →  EAGAIN / EWOULDBLOCK
A20_ERR_TIMED_OUT     →  ETIMEDOUT
A20_ERR_CANCELED      →  ECANCELED
A20_ERR_PROTOCOL      →  EPROTO
A20_ERR_RANGE         →  ERANGE
```

### 4.3 扩展规则

新错误码只能追加。已定义的错误码语义不能改变。如果现有错误码的语义过于宽泛，应定义新错误码并在 `abi_minor` 中标记新增。
