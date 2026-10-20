# Linux keyring

## 既有子系统契约

`kernel/abi/linux/sys_keyring.c` 负责解码 Linux syscall 参数、做边界检查、拷贝输入字符串/负载，并执行输出
usercopy，然后调用 `kernel/include/ipc/keyring.h` 里的 `keyring_*` API。`kernel/ipc/keyring.c` 中可选的
C 参考实现持有一个全局的 key/keyring 对象序号映射、一把粗粒度 irqsave 锁、keyring 成员关系与引用、
任务 session-ring 挂钩，以及一个按 uid 持久化的 ring 缓存。

当前实现的命令有：add 或 replace、request、get keyring id、join session、chown、设置权限掩码、设置
超时、link、unlink、search、describe、read。fork 共享任务的 session-ring 引用，exit 释放它。request 先
搜 session ring 再搜 user ring，并可能尽力把找到的 key 链接进请求的目标 ring。search 按配置深度遍历
嵌套 ring，忽略已过期的 key。重复 link 成功且不再增加引用；对非成员做 unlink 返回 `-ENOENT`。修改属性
要求当前有效 uid 拥有该对象，或具备 `CAP_SYS_ADMIN`。存储的权限掩码目前还没有用于约束 read/search/link。
`KEYCTL_CLEAR`、`KEYCTL_INVALIDATE` 及其他未实现的命令继续返回 `-EOPNOTSUPP`；request 的 callout 数据
被忽略。

有一处遗留细节与 Linux 格式的 description 契约有意区分开：当以 `buf == NULL` 查询内核 helper 时，C
参考实现按每个 uid/gid 字段十一个字节来估算长度。对普通的十进制 id，这个估算大于实际渲染出的字符串；
真正渲染的那次调用返回实际字节数。Rust API 镜像这个 helper 专属的估算，以保持当前 syscall 的长度查询
结果一致。guest 测试应把它记为兼容性怪癖，而不是正确性判据。将来若要改变这一行为，必须同时更新两个
selector 与 syscall 契约测试。

超时换算把乘法与加法饱和在 `u64::MAX`，因此极大的非零超时不会回绕成立即过期；零则清除过期时间。
惰性创建特殊 ring 仍然无法暴露独立的 `ENOMEM` 结果，因为它的公开 helper 只返回空指针，调用方只能把
该失败映射为 `ENOKEY`。这是既有 API 的限制。存储的权限掩码会保留且可修改，但完整的 `KEYCTL` 权限位
约束没有实现；已实现的属主/`CAP_SYS_ADMIN` 检查仍然生效。因此本模块不宣称完整的 Linux keyring 语义。

## 本批中 Rust 承担的职责

`kernel/ipc/keyring.rs` 拥有全部语义状态与决策：key 与 ring 的分配、序号映射与序号分配、对象引用、
多对多成员关系节点、特殊序号解析、session 与持久 user-ring 生命周期、过期、查找、搜索深度、属主/
能力检查，以及全部 API 操作的结果。它导出 `a20_rust_keyring_*` 核心入口。
`kernel/ipc/keyring_rust_bridge.c` 把既有的公开 `keyring_*` ABI 以及 fork/exit 挂钩转发给 Rust。

桥接层只包含机械式内核访问：当前任务与凭据字段、能力查找、session 槽 get/set、irqsave 锁的加解锁、
单调纳秒、以及 slab 分配/释放。Linux syscall 文件继续拥有 syscall 线格式解码与用户指针，不重复对象策略。

Rust 核心保留 C 参考实现以便对照。源码不含架构专属指令；最初声明为生产验证目标的只有 RV64。不要因为
源码采用架构中立的写法，就推断其他目标构建或部署档位已经通过。

Rust 的安全契约在一把 irqsave 锁之下强制执行。序号映射是非拥有式索引；每个映射中的存活对象，其引用
来自任务 session 槽、持久 user pin 或包含它的 ring 条目之一。ring 条目对每个父节点唯一，且构成环的
ring 链接返回 `-EDEADLK`，因此归属图是一个 DAG。最终释放先移除该对象的序号条目，再用一个侵入式
worklist 迭代排空其子引用，拆除的栈深为常量。分配与负载拷贝都在发布之前完成。链接失败会回滚新 key
的映射条目，而替换则在一次加锁步骤中换入一个已完整分配的负载。加入替换后的 session 会先发布新 ring，
再放下任务持有的旧引用；在继承/成员引用仍然存在期间，旧 ring 仍可通过序号解析。带检查的引用递增失败
返回 `-ENOSPC`；ABI 为 `void` 的生命周期挂钩在取不到 fork 引用时保持子槽不变。裸指针被限制在加锁核心
与机械桥接层内，syscall 用户指针留在 ABI 适配器里。访问对象的桥接函数只在 Rust 调用方持锁期间被调用；
凭据读取与分配/时间原语是叶子操作。`object_destroy` 只用于发布之前销毁空对象，或在候选 key 已从映射
移除之后；已发布且已填充的 ring 只通过迭代式 final-put 路径回收。损坏的负引用状态不会被再次递减。
`Item` 与 `Object` 的存储使用内核 slab 桥接层，其对齐足以承载这两个 C 兼容布局；Rust 保持对象布局私有，
桥接层从不窥探它。

每个 ring 最多保留八个已分离的 `Item` 节点供复用。unlink 先清空子指针并放下子引用，再缓存该节点；
link 在发布之前重新初始化缓存节点。溢出节点被释放，ring 最终销毁时释放其缓存。该池由同一把全局锁
保护，且不持有对象引用。

移植同时带两个有意为之的 C 参考缺陷修复：ring 成环被拒绝，而不是通过引用环泄漏；被替换的 session ring
在最后一个引用消失之前保持可索引。C 参考实现也改为迭代式回收，使深层无环拆除不再在内核栈上递归。
这些不作为与旧 C 缺陷逐字节兼容的承诺。

## 验证与剩余限制

本模块的 host 测试用仅测试用的内核原语桩驱动真实的 Rust 入口点；独立测试是
`kernel/ipc/keyring_host_test.sh`，经仓库的构建租约编译进 P3 私有构建树。

guest 验证在 QEMU SMP=2 下用 C 与 Rust 两个 selector 调用真实 Linux syscall，覆盖 add/read/describe/
search 成功路径、属主/能力拒绝、过期、link/unlink、非法与边界输入，以及资源释放。自包含的
`/bin/keyring_rust_test` 验收场景只有在以下检查全部满足之后才打印 `KEYRING_RUST: PASS`：替换保留 key
序号；直接与嵌套 search 以及 read/describe 返回预期值；非特权子进程无法修改父进程的 key；过期返回
`EKEYEXPIRED`；重复/缺失成员行为以及直接环与三环成环的拒绝都正确；旧 ring/key 在被继承的子进程持有
期间保持可索引，并在最后一次 unlink 或子进程退出之后被回收；超长/空/缺失目标输入返回既有错误。当前
`user_strncpy` 契约会把字符串静默截断到 `max - 1` 字节并补 NUL，因此 guest 检查超长的 key type 能在其
截断后的拼写下被搜到，并用另一个非法指针触发 `-EFAULT`。它不把这个 helper 行为当作 Linux 理想的
长名字语义。

该验收二进制只做正确性检查。曾有一次独立的固定迭代实验在完全相同的 guest 二进制上测量 add/update、
直接 search、read 与 link/unlink，并在五次成对 QEMU 启动中交替 selector 顺序；其原始日志留在仓库之外、
现已不可得，记录的摘要显示时序波动很大且各阶段方向不一致，因此没有确立整体加速。它不属于当前验证。
