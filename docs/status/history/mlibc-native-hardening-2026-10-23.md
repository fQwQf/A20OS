# 2026-10-23 mlibc Native 移植加固与验收

10 月 23 日记录对应当时的未提交工作树，不能用后续仓库 HEAD 代替历史制品身份。最终结果为：**已具备实际软件可用性，仍有明确标准语义缺口，尚未达到完整 POSIX 或上游提交就绪**。证据保存在`build/validation/mlibc-hardening-20261023/`，每次 VM 运行的 `report.json`记录 kernel、镜像、程序和驱动哈希、逐例退出状态及自然关机结果。失败日志保留。这里的“通过”不表示完整 POSIX 或所有架构可用。各项运行证据针对明确的制品，不应把注册入口数量当成标准覆盖率。

## 设计约束

Native ABI 修改须遵守 handle/capability、稳定结构、显式协商和双 ABI 分层。新增规范见 [Native 1.1 运行时扩展](../../native-abi/10-runtime-extensions.md)。中间的 raw socket-option 三元组/raw sockaddr 形式已撤换；旧 exit/exec 不解释新增的未使用寄存器。POSIX fd、信号策略及运行时状态由 mlibc 翻译，内核不维护 libc fd 表。修改了 ABI，不能把本次工作描述成纯 sysdeps 修复或已经获批的上游移植。

## 已复现并修复

- 初始化 handle table 的 Native pending/mask，消除脏内存导致的随机致命通知；用 poisoned allocator 的 C/Rust host 对照覆盖，随后实际 VM 重复运行验证。
- clone 显式能力清单、失败事务回滚、rights/时态/标签继承；对子 VM 的输出改为遵守 COW 和写权限的核心复制路径，避免直接物理 memcpy 污染父进程。
- 真正的 byte-stream pipe endpoint：共享队列、EOF、非阻塞别名、4096 字节内原子写； Native 返回 closure 状态，SIGPIPE 默认/忽略策略在 libc。
- private/shared 匿名及文件映射与 COW、定位 I/O 不改变共享 offset、向量操作不静默截断。
- fd 900/901、CLOEXEC 和失败 exec、长 argv/envp、父子能力输出隔离、密集 fork。
- 线程 kernel join、detached 栈释放与延迟 TLS/TCB 回收，保护页及调用者提供的栈生命周期。
- socket/accept/socketpair 临时内核 fd 泄漏，Unix 流对端关闭状态、datagram 原子性、 scatter/truncation/peek/zero-datagram，以及实际 TCP 半关闭 FIN 和异步 SO_ERROR。
- 基于实际 terminal 对象的状态/几何访问，不再把重定向文件当 tty。
- 类型化文件范围锁与任务/对象生命周期；SQLite 使用默认 Unix VFS/WAL，无自定义绕过锁的 VFS。
- wait-any 扫描全部子任务；wait/kill 复制能力固定身份，并发 wait 防止槽位/PID 复用混淆，创建前预留登记槽；普通处理器/SA_RESTART 的中断语义。
- exec 的 libc 私有状态恢复：子 TASK 能力、SIG_IGN；捕获动作复位、mask 保留。
- SDK 基线发现 joinable 默认值回归：恢复 legacy flags=0 自动回收，feature 15 显式请求 joinable，mlibc pthread 选择该选项；旧 Native deepen/IPC 等不承担 POSIX pthread 生命周期。
- fd 表扩容的读者使用受锁保护的快照，避免另一线程释放旧表；实际边扩容到 192 个描述符边 pread/fcntl 的压力测试覆盖此路径。SDK SET_LABEL 修正为版本化参数，构建依赖覆盖全部 uapi 头。
- A20 专用 rtld entry 独立于通用 Linux entry，避免端口入口逻辑污染其他 sysdeps。
- AArch64 QEMU virt 的 virtio IRQ 枚举使用完整 GIC INTID，修复真实网卡接收无法触发的问题；抓包保留失败证据。

## 运行证据

以下最终检查使用显式 joinable 修复后的 libc；不能混用依赖中间默认值的旧程序。 RV64 为 `kernel-build-46.log` + `apps-build-71.log`；四核内核为`kernel-rv64-smp4-build-02.log`。AArch64 libc 为冻结构建 12，sysroot/构建清单已复制到证据目录的 `final-aarch64-*`，不依赖保留 `/tmp` 才能检查制品。

| 验证 | 证据 | 结果与范围 |
|---|---|---|
| RV64 SMP2 最终完整回归 | final-rv64-run-50/report.json | 9 组应用/边界 + 23 个上游 ANSI，各 2 轮，64/64 通过 |
| RV64 SMP4 最终回归 | final-smp4-run-52/report.json | semantics/threads/signals/dynamic/network 各 2 轮，10/10 通过，线程使用四 CPU |
| RV64 单核最终回归 | final-up-run-53/report.json | 同上五组各 2 轮，10/10 通过 |
| RV64 最终实际软件 | final-software-run-51/report.json | Lua、SQLite 集成、SQLite CLI 各 2 轮，6/6 通过 |
| AArch64 SMP2 最终完整回归 | final-aarch64-run-08/report.json | build 09 内核；五组边界/网络 + Lua/SQLite 三例，各 2 轮，16/16 通过，含真实 HTTP |
| AArch64 单核补充 | final-aarch64-up-run-10/report.json | 线程与真实 TCP 两项通过，验证修正的 INTID 在单 CPU 下工作 |
| AArch64 实际软件独立检查 | final-software-aarch64-run-06/report.json | Lua/SQLite 三例各 2 轮，6/6 通过；板级 IRQ 修复前 build 08 |
| AArch64 旧 Native SDK 基线 | final-aarch64-native-run-09/report.json | 6/6 通过，双 ABI 行为与独立 expectation 一致（aarch64-native-linux-behavior-diff-02.log） |
| 旧 Native SDK 基线 | native-baseline-run-49/report.json | handle/contract/deepen/IPC + Linux/Native ABI behavior，6/6 通过；flags=0 仍自动回收 |
| 双 ABI 行为对照 | native-linux-behavior-diff-01.log | Linux/Native 输出与独立 expectation 一致 |
| wire layout | native-layout-final-09.log | kernel/SDK/mlibc × RV64/RV32/AArch64，九项通过；RV32 仅布局 |
| Native thread admission | native-thread-admission-final-06.log | x86_64/AArch64/RV64，legacy 默认与显式 joinable 均通过；host adapter |
| Native host | native-runtime-host-05.log | x86_64/AArch64/RV64 + x86_64 sanitizer 通过；依赖 mock，不替代 boot |
| 能力表 | handle-slot-final-04.log | C/Rust 两实现、毒化分配、temporal/ref/quota/race 通过 |
| feature 协商 | native-features-final-02.log | 生产 abi_info/feature_test 的 NATIVE/BOTH profile 一致性、1.1 版本与未定义索引拒绝通过；host 64 位运行 |
| 分层与覆盖 gate | abi-gates-final-09.log | ABI coverage 1、boundary 19 项断言通过 |
| Linux mlibc 对照 | linux-tests-final-05.log | 439 pass，4 expected fail，1 skip，1 fail；不计入 A20 运行覆盖 |

AArch64 `final-aarch64-run-05` 的本地网络前置检查通过，但 host-network 两轮均失败，不能算完整通过。`aarch64-network-diagnosis-07/network.pcap` 显示已发 ARP 并收到 QEMU 应答，客户机未消费。检查 QEMU 实际导出的 DTB，virtio slot0 为 `(SPI,16,edge)`；板级枚举误把相对 SPI 16 当完整 INTID 16。修复为 `32+16=48`，对应 slot4 网卡 INTID 52，不修改 Native ABI，也不把 IRQ 问题绕进 sysdeps。修复后 `final-aarch64-run-08` 的 16/16 项通过，包含两轮真实 HTTP/拒绝连接；`network.pcap` 保留 ARP/TCP 交换。旧失败记录仍保留，不能改写成通过。

Linux 唯一失败 `posix-aio` 已用 `linux-aio-trace-01.log` 单独追踪：QEMU 用户模式显示`Unknown syscall 425`（io_uring_setup），随后触发现有 Linux aio 实现的断言。这项未通过，不能把 Linux 完整测试报告写成全绿。

Lua 5.5.1 / SQLite 3.54.1 使用官方源码：[Lua FTP](https://www.lua.org/ftp/)、[SQLite 下载](https://www.sqlite.org/download.html)。`tools/build-mlibc-software.py` 记录源文件、编译命令、配置和 libc 哈希；源码未做 A20 特判。 Lua 启用 POSIX，未启用 dlopen/readline；SQLite 开启 THREADSAFE、Unix VFS、pread/pwrite，关闭 load_extension。测试覆盖文件、协程/GC、事务/回滚、跨 exec 写锁冲突、双线程连接、WAL、 checkpoint、重开及 integrity_check。软件版本与构建配置必须连同结果一起报告。

## 当前不能声称完成的内容

- Native 捕获通知仍在显式检查点运行，不能满足任意指令边界的 POSIX 异步处理器语义；无完整 job control、停止/继续 wait 报告、signal sender 信息和 altstack。
- console canonical 模式、PTY pending-output drain 及部分终端控制仍有限。
- 部分可选 sysdep/资源限制接口仍返回不支持或有历史简化；没有对所有公开 libc API 做逐项一致性证明。
- 子任务登记上限 1024；多线程 fork/exec 的全部 libc 内部锁和取消边界仍需额外压力验证。
- Native 旧 bind/connect 地址面仍有历史债务；本次不以 mlibc 可运行为理由整体搬入 POSIX 网络 ABI。
- RV32 仅做布局检查，x86_64/LoongArch/ARM 的完整 Native mlibc boot 未验收；QEMU 不能替代物理机。
- 最后若干 detached TCB 的延迟回收需下一次线程操作或进程退出；不能声称全场景即时零残留。

## 上游贡献准备

可以整理成可评审的贡献，但当前工作树不是可原样投递的大 PR。应分开：

1. A20 内核/SDK 协议修复与规范；不属于 mlibc 上游。
2. sysdeps/a20、独立 rtld entry、Meson/交叉构建与目标运行测试。
3. 通用线程 join/detach 钩子、TCB/TLS 析构、pthread_getattr_np、preadv/pwritev 等独立修复，分别给出不影响其他端口的依据和上游测试。

上游 mlibc 贡献规范要求逻辑独立且可构建的提交、测试套件和维护者评审；完整 CI 覆盖多架构/编译器/静态共享/选项配置。当前只满足其中一部分，不保证维护者接受；未创建提交、PR 或向外发布。

最终报告及当前源码哈希索引为证据目录的 `final-validation-manifest.json`。它包含工作树原有修改的身份，不是本次修改作者清单。10 月 24 日按下节复核范围整理为本地提交；此前冻结制品及哈希不因此改写。

## 复验入口

冻结源码构建工具支持静态/共享及 RV64/AArch64，不下载依赖：

```sh
python3 tools/build-mlibc-native.py --arch riscv64 --library both --jobs 6 --output /tmp/a20-recheck-rv64
python3 tools/build-mlibc-native.py --arch aarch64 --library both --jobs 6 --output /tmp/a20-recheck-aa64
make check-native-abi-coverage check-abi-boundary
python3 tools/check-native-exec-features.py
python3 tools/check-native-runtime-layout.py
```

需要对应的交叉编译器/C++ 头和 Meson/Ninja；本机 AArch64 C++ wrapper 的命令、编译器版本/哈希及参数见 `final-aarch64-build.json`，布局检查可用 `--aarch64-cxx` 指定。应用/软件构建命令分别保存在 apps build 日志、`final-aarch64-apps/commands.json` 和`final-software-{rv64,aarch64}/build.json`。原版软件源码下载来源与校验值在`final-software-sources.json`。构建成功本身不写作运行通过。

可重放冻结应用的一例（选择新的空输出目录，不覆盖验收证据）：

```sh
python3 tools/check-mlibc-runtime.py --arch aarch64 \
  --kernel .kernel-build/mlibc-hardening-aarch64/kernel.elf \
  --image .kernel-build/mlibc-hardening-aarch64/fat32.img \
  --apps build/validation/mlibc-hardening-20261023/final-aarch64-all-apps \
  --shared-sysroot build/validation/mlibc-hardening-20261023/final-aarch64-sysroot \
  --case semantics --case threads --case signals --case dynamic --case network --case software \
  --host-network --capture-network --cycles 2 --output /tmp/a20-recheck-runtime
```

runner 的 `report.json` 给出实际 QEMU 命令、kernel/image/应用哈希，检查退出状态和自然关机。若重建了源码或内核，必须生成新记录；不能把历史制品通过替代当前构建验收。

## 2026-10-24 设计复核与提交验收

按用户要求再次对照 00-overview 核心原则、01-types 的 E-APPEND 和 09-native-abi-deepening 的机制三分复核。**本次新增接口满足这些设计约束**；这不是对整个历史 Native ABI 的形式化证明，也不取消上文列出的语义缺口。

| 设计约束 | 本次实现与复核依据 |
|---|---|
| 对象与最小权限 | 资源操作经 handle lookup、对象类型和 rights 校验并 pin 引用；clone 显式清单保留 rights/时态/标签，失败回滚；不新增全局 PID/fd 授权入口 |
| 类型化机制 | socket/terminal/range-lock 为固定 Native record；未知选择拒绝；内部调用核心 net/tty/locks API，不接收任意 ioctl/fcntl 请求 |
| 版本与布局 | 1.1 版本与 feature 5..15 显式协商；start_info v2 追加 v1 前缀；recv v2 不占旧 padding；kernel/SDK/mlibc 九项布局检查通过 |
| 兼容性 | scalar exit/exec 保持忽略未用寄存器；flags=0 线程自动回收，joinable 显式选择；旧 SDK 两架构各六组通过 |
| 双 ABI 分层 | Native 不调用 Linux syscall 实现；核心使用内核自有参数；POSIX fd/errno/termios/消息转换及 exec 私有状态留在 mlibc |
| 不夸大支持 | 不支持的 VMO private、辅助能力消息、终端 drain 等明确拒绝或记录限制；旧 bind/connect 与 file flags 数值契约作为历史兼容面保留 |

复核补齐 GET_TERMINAL_CONFIG 对 reserved_byte 和 reserved_tail 的拒绝，并新增实际 Native 调用回归；SDK/内核新头及新增 C 测试按仓库格式整理。另发现 bare-metal GCC 不把 `-static` 传给 ld：当 sysroot 同时安装 libc.a/libc.so 时可能选错库，静态应用 recipe 现显式传 `-Wl,-Bstatic`，已通过同目录静态/共享产物的构建及运行验证。

为排除工作树中其他待提交修改的依赖，将本次选择的文件叠到原始基线 `106c5d73`（另含已单独提交的 AArch64 IRQ 修复），导出到独立目录后重新构建。未把 COW/块缓存/调度性能、快速启动等原有待提交修改带入该目录。证据位于 `build/validation/mlibc-commit-review-20261024/`：

- `isolated-dev-rv64.log`、`isolated-dev-aarch64.log`：两架构 kernel/rootfs/SDK 从独立源码构建通过。
- `isolated-libc-rv64-02.log`、`isolated-apps-rv64-02.log`：RV64 静态/共享 libc 和应用重新构建通过；Meson 依赖使用已固定的本地源码，无下载。
- `isolated-runtime-rv64/report.json`：9 组应用/边界、23 个上游 ANSI、6 组旧 SDK/双 ABI、3 个软件用例，各两轮，共 **82/82** 通过。
- `isolated-runtime-aarch64-03/report.json`：5 组边界/动态/网络、6 组旧 SDK/双 ABI、3 个软件用例，各两轮，共 **28/28** 通过；semantics 为本次重新编译，其余 libc/应用使用源码一致的已冻结产物。
- `isolated-network-aarch64-02/report.json`：真实 HTTP/拒绝连接四轮 **4/4** 通过，保留 pcap。
- `isolated-runtime-aarch64/report.json` 的首次复验为 **27/28**：一次 HTTP 接收等待在 5 秒超时；保留失败，不以随后的通过消除其偶发性。该问题未确认根因，不能声称网络全场景稳定。
- `isolated-layout.log`、`isolated-features.log`、`isolated-thread-admission.log`、`isolated-filemap.log`：布局、协商、旧/新线程准入和定位映射检查通过。
- `isolated-gates.log`、`isolated-docs-02.log`：ABI coverage/boundary/config 与文档检查通过；生成清单只按所选提交内容更新，手工 requirement 状态未改写为完成。
- `isolated-format-02.log`：完整格式门禁仍因 353 个未在豁免表中的既存文件失败；已逐字节确认这些文件与 `106c5d73` 一致（`format-scope.json`）。本次所选文件无新增不合规项，未扩大豁免表。

提交在 `fix/native-mlibc-runtime` 本地分支，分为板级中断、Native 协议/SDK/规范、 mlibc 通用生命周期、A20 sysdeps、构建测试与验收记录，以及生成清单更新；未推送远端。
