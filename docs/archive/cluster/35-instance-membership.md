# 运行实例成员准入

## 统一机制

[34](34-runtime-namespace.md) 的 instance/domain/boot_epoch 现在通过同一发现协议和成员状态机参与服务选择。原生共享 RAM 内核是一个执行域，无论它包含多少物理 MCU；独立服务器内核也是一个执行域。ISA、板卡和 UART/UDP 不参与实例相等判断。不同 ISA 的域能够提供公共工作服务，原生线程的寄存器/栈迁移仍须满足 [27](27-portable-domains.md) 的相容性契约。

这是统一运行状态的成员基础：绑定的控制进程只将同一实例、匹配目标域的记录纳入可选择视图。`BOUND` 仍是配置状态，`ALIVE` 是有期限的可达性观察；两者都不表示所有域同时健康或某个应用已经启动。各控制进程的观察可以暂时不同，本阶段没有强一致全局快照、全局对象目录或统一应用生命周期。

## CD/2：发现实例身份

固定发现槽位 `0xfffffffe` 不变。CD/1 请求 16 B、回复 128 B 保持兼容。 CD/2 请求同为 16 B，只把版本字节置 2；回复 192 B：

| 偏移 | 内容 |
|---|---|
| 0–127 | 既有目录记录，版本字节为 2；generation/索引/服务语义不变 |
| 128–191 | 显式小端编码的 64 B runtime_info |

runtime_info 的字段布局见 [34](34-runtime-namespace.md)，编解码函数为`a20_runtime_info_encode/decode`。UUID 按原始字节传输，整数为小端，所有保留字段为零，解码失败不发布输出。BOUND 的 domain_id 必须等于目录的 node_id。 UNBOUND 必须携带全零 UUID 和 epoch，不能从 route、地址或 CL nonce 推断身份。

目录中的 boot_epoch 是既有 CL **传输会话**，附加快照中的 boot_epoch 是**内核运行实例启动代次**。它们不是同一个字段，不能互相代替，也不要求数值相等。Native 内核直接读取 core/runtime 快照，目录不依赖 clusterd 寿命。`a20_cluster_runtime_query` 返回实际快照及可选传输代次；收到 CD/1 的旧对端返回 NOT_SUPPORTED，不能假定它属于本实例。

静态 MCU leaf 使用相同编解码器。可信引导层可以在 HELLO 前调用`a20_mcu_leaf_publish_runtime` 发布只读快照：域须匹配 leaf 身份，完全相同的重试成功，不同配置拒绝，已开始握手后禁止首次发布。该适配器不是第二个 Native 内核绑定源；独立 leaf 默认为 UNBOUND。当前板级启动路径尚未自动从原生 runtime 绑定向 leaf 发布，也没有持久实例 epoch 提供者。

可靠 UART 回复总长为 `32 + 20 + 192 + 2 = 246 B`，在 256 B MTU 内，无需扩大 MCU 帧缓冲或加入分片。实际配对测试覆盖该路径、收据释放与旧 CD/1。

## CM/2：控制面证明

CM/1 96 B 请求、80 B 回复保留。CM/2 请求仍为 96 B，版本 2；回复为 144 B：既有 80 B 记录后附加目标域的 runtime_info。成功回复必须携带已检查的 BOUND 快照，domain_id 等于记录中的目标节点；失败回复附加合法 UNBOUND 快照。请求 cookie、工作能力、服务槽位和 incarnation 语义不变。

绑定控制进程对旧 CM/1 客户端也执行同样的实例过滤，普通`service://any/work` 接口无需修改。CM/2 用于查询目标归属和间接探测。间接证明必须同时匹配尚未超时的 probe cookie、已确认的传输代次和实例启动代次；助手不能首次接纳一个域，也不能撤销显式的实例拒绝。gossip 只是路由线索，不具备成员准入权；绑定控制进程只广告已接纳成员。

## 状态与故障

- 未绑定的控制进程保持旧发现/成员策略，使用 CD/1。所有新状态位默认为零。
- 绑定控制进程只用 CD/2，拒绝未绑定、错误实例、域不匹配和畸形回复，禁止降级。
- 一个目录分页轮次的传输代次和实例启动代次必须保持不变；END 后才原子发布完整服务目录。实例检查失败立即撤回该域服务并进入 SUSPECT，不尝试间接路径把不匹配记录恢复为可用。
- 同一已知传输启动不允许改变 runtime epoch；新的传输启动不得复用最后确认的 runtime epoch。拒绝后保留该 epoch，防止下一次重试绕过检查。
- 内核从 UNBOUND 首次绑定时，clusterd 在下次控制请求分派前及周期刷新时清空先前成员和服务视图，保留单调 probe cookie，再按新实例重新探测。
- 控制进程重启重读内核绑定，重新发现同实例服务，不改变内核身份。

运行实例 epoch 是不透明的唯一启动名，本实现没有把它改成有序时钟。成员只保留有界当前记录；墓碑淘汰、控制进程重启和接收端重启后没有完整历史。因此它只能拒绝与保留记录矛盾的重用，不能证明任意历史 epoch 未复用，部署仍须使用可靠持久分配器。两个同时使用同一域身份的内核也需要后续域租约/唯一持有者协议。本机制是归属与生命周期检查，当前传输没有身份签名。

## 通用时间预算

慢速 MCU、模拟器或繁忙服务器不能被固定的 700 ms 目录期限排除在机制之外。 clusterd 新增可选、与 ISA 无关的 `--timing-scale N`（放在 node 后、种子前），配置文件新增 `timing-scale N`，监管者将它传给控制服务。N 为 1–16，省略为 1；旧配置和时间行为不变。例：

```text
version 1
node 00000000000000000000000000000001
role worker
timing-scale 10
```

它成比例放宽直接/间接请求期限、成员探测/怀疑/墓碑期限和服务租约；探测重试及路由刷新周期不变。不自动按 STM32、ARM 或资源档选择不同策略。实际混合 ISA 验收全部节点都用 N=10，允许功能先完成，不要求目标达到亚秒响应。 RPC 最长仍小于公共 SDK 的 30 s 上限；租约和故障检测变慢是显式部署取舍。

混合测试也暴露 AArch64 异常返回窗口中的嵌套 IRQ 竞争。C 处理函数可能调度后带着开启的 IRQ 返回，汇编随后恢复 ELR/SPSR、寄存器并释放 SP，期间再入会破坏正在恢复的帧。内核态和用户态返回均先屏蔽 IRQ，由 ERET 原子恢复保存的 PSTATE。修复前日志保留了恢复 FPSR 时访问 0x338 的实际内核页错误及原 ELF；这不是通过增大业务超时掩盖的故障。该改动对普通 ARM 返回路径增加一条屏蔽指令，因正确性所需；双核 Native 同步/分配器/线程/运行实例回归另行执行。

## 兼容性、成本与验收

普通 syscall、调度、同步、内存和非集群接口没有变化。新增工作位于集群发现及控制面，CD/1 在线格式不变；CD/2 每条记录增加 64 B。启用 leaf 的固件增加 64 B 静态快照；成员表每项增加两个状态字段，控制进程增加实例快照和探测代次。不能把这些成本描述为零。默认 QEMU 精简 MCU 未启用 leaf，重建后的 kernel.bin SHA256 仍为`c6f1737ce1523db0c20caeb2334815fa624377f65530a6fe176b48a92914b21d`。本阶段没有新增性能优化，也没有完整吞吐/尾延迟无回归证明。

```sh
make check-cluster-unit check-native-runtime check-native-task-object
make check-cluster-instance
python3 tools/check-cluster-mesh.py
make smoke-native-cluster smoke-native-cluster-supervisor
```

`check-cluster-instance` 使用三套实际 RV64/AArch64/x86 内核、实际 Native agent/clusterd 和 UDP 帧转发：旧未绑定模式选择所有服务；首次绑定撤回外部实例；未绑定节点不可选择；重启复用 epoch 被拒绝；新 epoch 恢复；控制进程重启重新发现。服务调用核对 DOT 结果，抓取实际 CD/1、CD/2、CM/1、CM/2 帧。 AArch64 采用同一构建的 kernel.bin 启动，使 QEMU 的 DTB/x0 交接传入网络配置。命令、原始日志、有限帧追踪和实际帧计数写入 `.kernel-build/cluster/instance/`。

宿主测试直接执行生产 UAPI 状态机以及 leaf/UART 实现，I/O/时钟由适配器替代；它不是物理板证据。Minecraft 验收仍遵循 [24](24-single-system-minecraft.md)：还需要应用归属与全局对象代理、文件/显示/输入设施、Java 运行时，以及实际多 MCU 板级共享内存验证。本阶段不以成员准入测试代替客户端启动。
