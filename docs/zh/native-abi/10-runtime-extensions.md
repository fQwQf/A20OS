# Native ABI 1.1：运行时机制与兼容边界

本页规范 Native 1.1.0 运行时接口契约。`abi_info` / `execution_info` 的 Native 版本为 **1.1.0**；既有 syscall 编号保持稳定。实现、SDK、mlibc 与测试保持同步演进。运行证据与历史演进见 移植验收记录。

## 1. 设计约束与取舍

遵循 [核心原则](00-overview.md#核心原则) 与[机制三分](09-native-abi-deepening.md#1-机制三分清单)：

- Native 操作资源依赖 handle、对象类型、rights、时态约束及安全标签；不增加按全局 PID/fd 操作资源的入口。
- fd 编号、`FD_CLOEXEC`、`sockaddr`、`MSG_*`、`termios` 和 POSIX 信号默认动作由 libc 翻译。
- `handle_control` 的每个 op 有固定版本化结构；不允许任意 `level/optname/void*`、ioctl/fcntl 请求透传。
- Native 包装调用 ABI 无关的 VFS/MM/proc/net 核心 API，不调用 Linux syscall 实现；核心不包含 Native 用户结构。
- 旧调用的未使用寄存器仍被忽略；新结构字段不得占用旧版本尾部 padding。新增能力须查询 feature bit。

开发实践中避免直接暴露 socket option 三元组、raw sockaddr 或解释旧 exit/exec 额外寄存器的设计；相关逻辑严格收敛至版本化结构与 libc 封装层。

## 2. 协商与布局

新增 feature 索引（不是 bit mask 常量）：

| 索引 | 名称 | 契约 |
|---|---|---|
| 5 | BYTE_STREAM | 显式选择的 byte-stream pipe endpoint |
| 6 | MAP_MODES | 显式 private/shared 映射 |
| 7 | EXEC_BOOTSTRAP | 版本化 image replacement + 不透明运行时启动数据 |
| 8 | FILE_FLAGS | 类型化、共享 open-file 状态查询与更新 |
| 9 | THREAD_EXIT_CLEANUP | 版本化线程退出与自有栈释放 |
| 10 | SOCKET_OPTIONS | 固定字段 socket configuration，非任意 socket option 透传 |
| 11 | TERMINAL_CONTROL | Native terminal configuration |
| 12 | FILE_LOCKS | 文件对象/当前 task 容器范围锁 |
| 13 | SIGNAL_EXIT | 版本化通知原因退出及可区分的等待状态 |
| 14 | NET_MESSAGES_V2 | Native endpoint + Native 消息标志 |
| 15 | THREAD_JOINABLE | 显式保留可等待的线程退出记录 |

feature 7/9/13 的显式 64 位 scalar tag 当前仅用于 64 位调用 profile；32 位 profile 不宣告这些位，SDK 扩展 wrapper 返回 NOT_SUPPORTED，不能把 tag 截断后误作旧调用。 32 位仅验证记录布局；要提供同等操作，需要另行设计可表示的调用选择方式。

输入结构 `version=1` 要求完整 v1 前缀；更大的 size 只读取已知字段，未知 version、未定义选择位和保留位返回 `INVALID_ARGUMENT`。网络消息显式支持 v1/v2 两个最小前缀：

| 结构 | v1 最小 size | v2 最小 size | v2 追加字段偏移 |
|---|---:|---:|---|
| net_sendmsg | 64 | 72 | addr_length=64，reserved=68 |
| net_recvmsg | 72 | 80 | addr_capacity=72，out_flags=76 |

recv v1 的 68–71 是原有尾部 padding；v2 保留为零，不用来承载新输入。输出最多写入调用者声明的 size。新错误码只能追加；旧程序可能把新细分错误降级为通用 I/O 错误，不能假设旧 errno 映射已经认识新状态。

## 3. 对象 I/O 与映射

`channel_create` flags 缺省仍创建消息 Channel。显式 `A20_CHANNEL_BYTE_STREAM=1`选择 `A20_OBJ_PIPE_ENDPOINT`，`A20_CHANNEL_NONBLOCK=2` 选择非阻塞；未知位拒绝。 byte-stream 不携带 handle，只使用 handle_read/write，读写端分别具有 READ/WRITE，并具备 DUP、TRANSFER、WAIT、CONTROL 等被授予的权利。dup/fork 引用相同对象，最后 reader/writer 关闭产生真实 peer-closure/EOF。4096 字节内写入保持原子性。 Native 的 peer-closure 返回 `BROKEN_PIPE`；**SIGPIPE 策略属于 POSIX libc**。

handle_read/write 的 iovec 在进入内核时快照，单次操作保持流/消息边界；offset 指定位置时不改变 open-file 当前位置。管道拒绝定位，返回 `ILLEGAL_SEEK`。不通过临时 lseek 模拟 pread/pwrite。部分传输必须发布实际 out_count。

`vm_map` 新增 `A20_MAP_SHARED=1`、`A20_MAP_PRIVATE=2`，互斥；flags 不含这两位时保留既有行为。private 匿名/文件映射参与 COW；private 文件副本不写回源文件，源 READ 允许修改自己的副本，**不增加源 handle 的 WRITE right**。VMAR 约束仍生效。 MEMORY/VMO 的 private shadow 另属对象派生机制，当前返回 NOT_SUPPORTED；缺省 VMO 共享语义不变，不能为满足 mmap 测试隐式修改它。

## 4. 版本化退出与 image replacement

旧 `task_exit(int32 code)`、`thread_exit(int32 code)` 仍只读取 arg0，忽略其余寄存器。 arg0 为完整 64 位标签 `A20_EXIT_STRUCT=0x4132304558495401` 时，arg1 才是`a20_exit_args` 指针；该标签不可能是符号扩展后的 int32 code。

```c
struct a20_exit_args {
    uint32_t size, version;
    int32_t code;
    uint32_t cause, notification, flags;
    uint64_t release_base, release_length;
}; /* size=40 */
```

task_exit：cause=0 为普通 code，notification 必须为零；cause=1 为 Native 通知原因， notification 为现有通知编号 1..63。task exit 不接受 flags 或 release 区间。 thread_exit：cause/notification 必须为零；flags=1 表示 detached，允许提供当前地址空间中调用者自有、页对齐、无溢出的栈区间。内核在 kernel stack 上释放它，然后终止执行上下文。该原子生命周期机制不包含 pthread/TLS 知识；TCB/TLS 的回收属于 runtime。

task_wait 继续以 TASK/THREAD handle + WAIT right 指定稳定身份。THREAD handle 的通知与终止只作用于其执行上下文，不因缺省 mode 扩大到 TASK 容器；TASK 终止仍作用于整个容器。exit_reason 为 0=normal、1=notification、2=notification with core；普通 code（包括 128+notification）与通知退出可区分。兼容保留后者 exit_code=128+notification 的既有数字表示；libc 才编码 POSIX wait status。线程 JOIN 必须确认 kernel death，再释放用户栈/TCB。

`thread_create.flags=0` 保留旧版自动回收退出记录的默认值；只有显式设置`A20_THREAD_CREATE_JOINABLE=1`（feature 15）才保留记录供 task_wait 消费。mlibc 为 pthread 请求该选项，不能把所有旧 SDK 线程隐式变为 joinable。该选项也适用于 32 位 profile，与仅 64 位的扩展退出标签分开协商。

旧 execve(path,argv,envp) 忽略 arg3..5。arg0 为`A20_EXEC_STRUCT=0x4132304558454301` 时，arg1 指向：

```c
struct a20_exec_bootstrap_args {
    uint32_t size, version;
    uint64_t path, argv, envp;
    uint64_t data, data_size, close_handles;
    uint32_t close_count, reserved;
}; /* size=64 */
```

data_size<=65536，close_count<=4096。内核复制数据与 handle 清单，在 image replacement 成功提交时才关闭指定句柄；失败不提前关闭。内核不解释 fd、libc 的私有 manifest 或 POSIX CLOEXEC 位。成功 Native 启动通过 start_info **version=2、size=120** 传入 bootstrap_data/bootstrap_size，追加在 v1 的 104 字节前缀之后；旧字段偏移不变。运行时必须检查 version/size；新内核仍可启动不读取新字段的旧程序。

mlibc 私有数据的 v2 保存 fd 表、仍由本进程持有的子 TASK 能力及 SIG_IGN 位图；新映像恢复这些运行时状态，捕获处理器恢复默认。内核只搬运不透明字节，不维护 libc 子进程登记表或 POSIX disposition。私有数据解析仍兼容只含 fd 表的 v1。

## 5. 类型化 handle control

既有 op 2..9 的含义保留；op 0/1 不恢复透传。以下操作均要求对应 handle 的 CONTROL right，并先 pin 对象。没有任意用户 payload 类型逃逸口。

| op | 名称 | 固定结构/对象 |
|---|---|---|
| 10 | GET_FLAGS | a20_ctl_flags_args；file/device/socket/pipe |
| 11/12 | GET/SET_SOCKET_CONFIG | a20_socket_config；socket |
| 13/14 | GET/SET_TERMINAL_CONFIG | a20_terminal_config；实际 terminal file/device |
| 15 | FILE_LOCK | a20_file_lock_args；regular file |

GET_FLAGS 沿用既有 `a20_ctl_flags_args` 的 open-file flags 数值契约；SET_FLAGS 的有效修改掩码仅为 APPEND、DIRECT、NONBLOCK。此轮没有重新编码旧标志，也没有开放任意 fcntl 请求；新 socket/terminal 控制使用各自具名 Native 字段，不能通过 flags 绕过。

### Socket configuration

`a20_socket_config` size=80，结构字段见同步的 kernel/SDK 头。fields 每次选择一个固定组： OPTIONS=1、TX_CAPACITY=2、RX_CAPACITY=4、RX_TIMEOUT=8、TX_TIMEOUT=16、 KEEPALIVE=32（保留，当前 NOT_SUPPORTED）、HOP_LIMIT=64、IDENTITY=128、FAILURE=256。

OPTIONS 使用 option_mask 选择一个具名布尔属性：REUSE_ADDRESS=1、REUSE_PORT=2、 KEEP_ALIVE=4、NO_DELAY=8；options 只能包含被选择的位。容量为字节，timeout 为 uint64 纳秒，hop_limit 为数值；identity/failure 只读。family 为 LOCAL=1/IPV4=2/IPV6=3， type 为 STREAM=1/DATAGRAM=2/SEQUENCE=3，transport 为 LOCAL=0/TCP=1/UDP=2。 failure 是 **A20 状态码**（零表示无错误），查询消费一次共享 pending error。不传 Linux errno、不接受 fd-valued 选项。未知 POSIX option 在 libc 返回 ENOPROTOOPT。

### Terminal configuration

`a20_terminal_config` size=60：size/version、action/reserved、input_modes/output_modes/ local_modes/device_modes/baud_rate、data_bits/stop_bits/parity/reserved_byte、17 个具名字符与 reserved_tail[3]。模式位名称与位置在 kernel/SDK 同步头逐项定义，**不采用 Linux 位图**。波特率为 bits/second，data_bits=5..8，stop_bits=1/2，parity=0无/1奇/2偶/3mark/4space。不暴露 Linux line-discipline 编号或 cc 数组索引。action 为 0立即/1排空后/2排空并清输入。

能力只覆盖已有核心行为；PTY 有未排空输出时的 drain 当前 NOT_SUPPORTED，console 的 canonical line discipline 仍不完整。保存并返回某个模式不代表完整实现全部终端处理。普通文件/管道不能假装成 tty，也不返回伪造的 24x80 几何。

### File range locks

operation=0查询/1尝试设置/2等待设置；owner_kind=0 当前 task 容器（handle table）作用域，=1 当前 open-file 对象作用域。type 使用 Native UNLOCK=0/SHARED=1/EXCLUSIVE=2；origin 为 0文件开始/1当前位置/2文件结尾；start/length 为有符号 64 位，length=0 延伸至 EOF。查询返回 owner_identity 仅为诊断元数据（对象作用域为 -1），不是可用于 task_kill/wait 的资源标识或授权。FILE 的 CONTROL 加 READ/WRITE 决定可建立的锁；不能凭诊断编号取得权限。

task 作用域锁不随 fork 清单复制；任一关联文件 handle 关闭或容器销毁会释放其 task 锁。对象作用域锁随相同 open-file 对象引用共享，最后引用关闭时释放。两种生命周期不能混用。这是显式的资源作用域选择；不接受原始 fcntl 命令或 struct flock。POSIX 类型/编号翻译在 libc。

## 6. Network message v2

v1 固定 IP record 保留；v2 使用 `a20_net_endpoint`（size=148）： size/version/family/length，uint16 port/reserved，uint32 scope，address[16]、local_name[108]。 port 为主机序；IPv4/IPv6 length 分别为 4/16，scope 仅 IPv6 有意义。本地 length 为 0..108 个名字节，可包含前导 NUL；不使用隐式 C 字符串长度。record 无 sockaddr 布局。

v2 flags：NONBLOCK=1、PEEK=2、WAIT_ALL=4、FULL_LENGTH=8；发送只接受 NONBLOCK。返回 out_flags：DATA_TRUNCATED=1、METADATA_TRUNCATED=2。内核不接受 POSIX MSG 位图。完整 endpoint 记录不做部分复制；libc 再实现 POSIX 地址缓冲截断及完整长度报告。

一次 datagram send 是一条消息，不按临时缓冲大小分片；stream 允许短读写。单次 stream 内核缓冲限 64KiB、datagram 1MiB，超大 datagram 明确报 MESSAGE_TOO_LARGE。附带能力传递需要独立 Native 控制格式，当前不接受 POSIX SCM_RIGHTS；收到未表达的辅助信息会释放其引用并报告 metadata truncation，不能泄漏 fd 或静默声称已传递能力。既有 bind/connect 等旧入口的地址形式属于原有接口债务，不因此声称整个网络 ABI 已完全重设计。

## 7. 验证与贡献边界

- `make check-native-abi-coverage check-abi-boundary` 检查入口与分层，不证明全部运行语义。
- `test_mlibc_semantics.c` 检查旧 scalar exit/exec 的非零未使用寄存器、类型化配置非法选择、版本拒绝、并发 wait 的稳定身份、exec 后子任务归属/忽略动作/信号掩码、实际 fd/对象生命周期、共享/COW、等待、锁、信号与线程回收。
- `test_mlibc_network.c` 检查 IPv4/IPv6/Unix、向量 datagram、truncation/peek、TCP/EOF。
- `check-mlibc-runtime.py` 保存 kernel/image/program 哈希、输出、退出码及自然关机结果，支持 RV64/AArch64；可选 host-network 路径的失败同样必须保留，不计为通过。
- 可贡献给 mlibc 的是 sysdeps/a20、构建接入及有独立依据的通用修复；A20 内核协议不由 mlibc 上游批准。应分拆评审，不能以本地软件运行替代上游设计/维护者接受。
