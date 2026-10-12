# 配置普通应用并从集群命令行执行

## 部署清单

`appd --manifest /etc/apps.conf` 从普通 Native 文件接口读取完整清单，验证成功后
才导出应用服务。原来的七参数分组仍可使用。两个入口共享同一 app owner、
ISA 镜像匹配、负载采样和真实任务生命周期；CA/1 协议与已有 SDK 均未改变。

清单以 `A20APPS1` 开始，每个 `app` 块选择一个镜像变体：

```text
A20APPS1
app c5000000000000000000000000000005 /bin/myapp-rv 243 64 1 3 2
arg --data
arg /assets/example folder
env HOME=/home/a20
env EMPTY=
end
```

`app` 行依次为 UUID、绝对镜像路径、ELF machine、指针位数、字节序、调用 ABI
和必需特性，与原七参数分组一致。`argv[0]` 为该镜像路径，`arg ` 后的整行是
一个额外参数，保留空格、空字符串和 UTF-8 字节。`env ` 后的整行为一项
`NAME=value`，允许空值；不继承 appd 的环境。不执行 shell 展开或转义。LF 和
CRLF 均可，参数内换行/NUL 尚不可表示。空行和以 `#` 开始的行是注释。

同一 UUID 可在多个块中配置不同 ISA 的镜像。管理员应为同一应用部署一致的
参数语义；当前不会跨节点比较清单内容摘要。清单在 appd 启动时读取一次，
运行中不热更新；更换清单需重启该控制进程，旧凭据随实际控制任务身份失效。

整个文件最多 16 KiB，最多 8 个变体，每个变体最多 16 个 argv（含路径）和
16 个环境项。变体不适配当前执行域时不会打开镜像。公共解析器位于
`kernel/include/uapi/a20/cluster_app_manifest.h`，不分配内存、不含板型或 ISA
条件；适配器提供持久的文本存储和 entry 数组。Native appd 直接把解析出的
argv/envp 交给真实 task_spawn v2，沿用 root/cwd/stdin/stdout/stderr 句柄。

## 实际命令行运行

可沿用 cluster-supervisor 的 `instance`/`boot-epoch` 配置完成实例绑定和路由部署，
见 [35](35-instance-membership.md)。各域显式启动本地 appd，之后已绑定到同一
运行实例且配置路由的节点可以执行：

```sh
/bin/appctl-run-rv c5000000000000000000000000000005 700 30000 101112131415161718191a1b1c1d1e1f 202122232425262728292a2b2c2d2e2f
```

参数依次为清单 UUID、非零十进制 nonce、启动预算毫秒、1–16 个候选域 UUID。
预算不超过 30 秒。新版命令通过 `a20_app_start_any_reusable` 选择活动程序数最少的兼容
域，并打印实际 owner、controller、client 和 nonce。它保持调用任务存活，
查询真实根任务直到退出，把实际退出码作为命令退出码返回。启动预算不限制
程序运行时长；收到 Native SIGINT/SIGTERM 时向原 owner 提交根任务停止，
等待实际退出后返回退出码；停止请求或回复丢失时保留停止意图，只在原 owner
查询和继续提交相同停止操作。运行中只恢复原 owner，不向另一个节点重新启动。观察无法恢复
时返回 125，并明确报告结果未知；不将断连当作已退出。

程序输出仍使用所在节点 appd 继承的标准输出，尚未代理到发起命令的终端。
命令行当前选择已部署清单，不上传任意 argv/envp 或应用文件。已有客户端
身份约束意味着命令退出后不能换一个 CLI 任务继续使用原 ticket。appd 保留的
8 项记录容量不变，但新版 CLI 已在观察终态后回收 CA/2 记录，不再有正常
累计 8 次执行上限；协议兼容性和保留记录的边界见 [39](39-application-recycling.md)。

## 验证

```sh
make check-cluster-apps
python3 tools/check-object-directory-portability.py --kind app_manifest
make check-cluster-unit
```

实际 QEMU 验收保留原三 ISA 的 6 程序 2/2/2 均衡、丢回复和重启流程；随后通过
Native 文件接口写入清单并重启 appd。从 RV64 客体运行真正 appctl 可执行程序，
分别启动 RV64、AArch64、x86 的 ELF，子程序检查带空格、空、中文参数，两个
环境项及结尾 NULL，通过继承的根目录打开自身文件，并返回 37。宿主分别检查
三个客体的工作输出和命令行退出记录，不由宿主填写应用状态。随后向仍在运行
的命令发送 SIGTERM，验证 ARM64 上真实程序退出码 137 和 owner 活动数恢复为 0。

公共解析器的实际跨 ISA 执行、ASan/UBSan 与 ARMv7-M/RV32 freestanding 编译
另有报告；这证明解析机制可移植，不证明 MCU 已有常规文件或网络 appd 服务。
Minecraft 所需 Java 运行时和物理多 MCU 验收仍见
[24](24-single-system-minecraft.md)。

停止验收另外主动丢弃第一轮停止请求，直到命令报告原 owner 上的重试；随后
丢弃已接受停止的回复，要求命令仍观察到实际退出且活动计数归零。这样既覆盖
“尚未接受请求”，也覆盖“已接受但回复未知”，不会仅以信号已发送认定完成。

冻结源码的 `apps-inputs-stop-loss/events.json` 为 PASS：三 ISA 各有一次完整
参数/环境校验，三个命令实际返回 37；丢弃 5 个停止请求、3 个停止回复及
4 个启动回复后，停止流程仍返回真实退出码 137，活动数为 0。原六程序
2/2/2 均衡继续通过。完整报文、内核、应用 ELF、日志与源码摘要归档于
`.kernel-build/cluster/evidence/application-inputs-<commit>/manifest.json`。
