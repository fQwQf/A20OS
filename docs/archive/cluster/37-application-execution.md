# 实际程序执行与负载分配

## 可以执行的流程

`appd` 调用真实 Native ELF 装载和 task_spawn，持有根任务引用，以实际 task_wait 结果更新运行/退出状态。`a20_app_start_any` 在候选执行域中查询同一应用清单的兼容镜像和活动程序数，选择活动数最少的域，再发送一次启动请求。相同负载按候选列表顺序选择；新启动后负载随真实任务增加，所以后续启动分散到其他域。退出后释放任务引用、减少活动数，启动结果继续保留，避免重复启动。

这是按**活动程序数量**进行的功能性均衡。当前不按 CPU 利用率、内存压力或程序权重估算负载，也不自动迁移正在运行的程序。该策略首先保证程序能执行、能分配到多节点，后续才能在实际需求下加入更细的资源预算。

每个执行域使用相同 `cluster_apps.h` 状态机；ISA 变体由清单要求与实际 execution_info 匹配，板型和传输不参与调度规则。共享 RAM MCU 的多 CPU 属于一个执行域，域内线程继续由公共调度器执行；独立服务器内核是不同执行域。异构域各自装载匹配机器码，不能将跨 ISA 本机线程栈直接迁移。

## 部署和接口

服务器显式启动可选 `/bin/appd-<arch>`，每组参数为：

```text
manifest-UUID image-path ELF-machine pointer-bits byte-order calling-ABI required-features
```

同一 manifest UUID 可配置多个架构镜像，最多 8 项。Native ABI major=1，minimum minor=0；内核装载器仍独立检查实际 ELF、段、重定位与入口。无法匹配返回 NO_IMAGE；同一域匹配多个镜像返回 CONFLICT，不任意选择一个。例：

```sh
/bin/appd-rv c1000000000000000000000000000001 /bin/myapp-rv 243 64 1 3 2 c1000000000000000000000000000001 /bin/myapp-aarch64 183 64 1 2 2 c1000000000000000000000000000001 /bin/myapp-x86_64 62 64 1 1 2
```

每台机器只需部署自身实际使用的文件；清单中的其他 ISA 变体不会被打开。应用接口位于 `user/liba20rt/a20_apps.h`：

```c
a20_app_ticket_t ticket;
a20_app_result_t result;
/* domains 为预配置候选域 UUID 数组，manifest 为应用 UUID。
 * nonce 在当前客户端任务生命周期内唯一，调用者保存返回的 ticket。 */
a20_status_t status = a20_app_start_any(domains, count, manifest, nonce,
                                       30000, &ticket, &result);
if (status == 0 && result.state == A20_APP_RUNNING) {
    a20_app_session_t session;
    if (a20_app_resume(&ticket, 8000, &session) == 0) {
        a20_app_query(&session, &ticket, 4000, &result);
        a20_app_stop(&session, &ticket, 9, 4000, &result);
        a20_app_close(&session);
    }
}
```

也可用 connect/start 明确指定所属域。候选域目前由调用者提供，尚未自动从 clusterd 成员表生成。每次候选查询检查 CD/2 实例身份、传输代次、控制任务身份和兼容清单。扫描和启动共享总预算；最终启动携带已观察的活动数加一，owner 在串行派发中重新检查。并发请求改变负载时返回 BUSY/WOULD_BLOCK，保证该次请求没有启动，而不是根据已过时负载继续接受。暂不可达或不兼容的候选不会被选择。表满的域不再接受新应用，也不淘汰旧启动记录。

## 启动结果和退出

一次启动由 `(controller object_id, client object_id, nonce)` 定位。controller 是实际 appd 任务的稳定对象名，因此服务进程重启即改变该身份；不是仅依赖内核 boot_epoch。请求键对应固定 manifest，重复 START 返回同一根任务/结果，改用另一 manifest 则冲突。CA/1 每个 owner 最多保留 8 次启动，包括终态。新增 CA/2 的显式回收和持续执行见 [39](39-application-recycling.md)，原 CA/1 记录不会被隐式淘汰。跨重启持久记录按[62-release-scope.md](62-release-scope.md) 的决定不属于 1.0；重启后旧 ticket 失败关闭为 `EXPIRED`，不重放也不推断执行结果。

| 状态 | 实际含义 |
|---|---|
| RUNNING | 已装载并发布真实根任务；不表示 GUI/JVM 已就绪 |
| STOPPING | 已对根任务成功提交停止请求，仍等待实际退出 |
| EXITED | 已观察到根任务退出，包含实际退出码 |
| FAILED | 启动适配器确认未启动，保存负错误码，重复请求不重新执行 |
| LOST | 已启动但身份/任务状态不能可靠观察；不推断死亡，也不重新启动 |

root 字段是 [36](36-object-directory.md) 使用的全局任务名；身份观察失败时可以为空，即使随后观察到退出也不会编造 root。appd 目前没有自动把 root 注册到 objectd，应用的 query 通过 owner 持有的真实任务引用完成。启动的程序继承 appd 的 root_dir、cwd_dir 和标准输入输出句柄，沿用普通 Native 文件和读写接口。原七参数入口的 argv 只有镜像路径；新增清单入口支持配置 argv/envp，见 [38](38-application-launch-inputs.md)。客户端仍选择已部署清单，不上传任意参数。

SDK 在发送 START 前写回 ticket。超时是结果未知，调用者应 resume 原 ticket 并 query，或在同一 controller 上显式重试同一键。`start_any` 不会在未知结果后换一个域重新执行。控制进程或内核重启后原 ticket 返回 EXPIRED；目前不持久重放启动记录。服务器根任务使用已有 PARENT_BOUND 规则，控制进程退出向根任务发送父死亡信号；没有承诺能终止任意未托管后代或复制应用内存。

## CA/1 协议

请求 176 B、回复 192 B，显式小端；可靠 UART 总长分别为 230 B 和 246 B，均在既有 256 B MTU 内。op 为 HELLO=1、START=2、QUERY=3、STOP=4。

| 偏移 | 请求 | 回复 |
|---|---|---|
| 0–3 | `CA`、version=1、op | 原样头与 op |
| 4–7 | START 期望活动数+1（0=不限该条件）；STOP 信号；其余 0 | status |
| 8–15 | 非零 cookie | 原样 cookie |
| 16–23 | 非零启动 nonce；HELLO=0 | 原样 nonce |
| 24–31 | 保留零 | state、i32 exit_code |
| 32–95 | controller ID；HELLO 全零 | 当前 controller ID |
| 96–159 | client ID；HELLO 全零 | root ID；成功 HELLO 为 execution_info |
| 160–175 | manifest UUID；HELLO 可为零或查询清单 | 原样 manifest |
| 176–191 | 不存在 | capacity、occupied、active、保留零 |

HELLO 携带 manifest 时只查询镜像可接纳性，不启动。所有回复包含同次派发的 owner 容量与实际活动数。status 为 OK=0、INVALID=1、STALE=2、NOT_FOUND=3、 CONFLICT=4、NO_SPACE=5、NO_IMAGE=6、IO=7、BUSY=8。错误回复不携带任务状态。解码检查 cookie、nonce、manifest、身份、容量关系、状态和保留位，失败不发布部分输出。整个 owner 必须由适配器串行调用；本地指针、PID 和句柄不上线。

## 实际验收

```sh
make check-cluster-apps
python3 tools/check-object-directory-portability.py --kind apps
make check-cluster-unit
make CCACHE= PYTHON=<Unicorn/pyelftools Python> check-mcu-program-rtl
```

三台实际 QEMU 内核分别为 RV64、AArch64、x86。相同清单包含三个 ISA 变体，测试通过同一接口启动真正 ELF、检查自然退出码 37、停止、重复启动、失败记录和控制进程/内核重启。负载流程自动启动 6 个程序，验证实际活动数为 2/2/2；子程序实际执行求和检查、通过继承的 root_dir 打开自身 ELF，然后输出校验结果并持续计算，宿主另检查三个客体的工作输出。宿主丢弃已接受启动的回复，验证原键查询能找回任务，显式重试仍是同一根任务。并发负载变化通过真实服务返回 BUSY 路径检查，不仅是宿主表测试。

MCU 固件的明确测试适配器使用相同生产 owner/CA/1，start 回调调用真实 mcu_program_spawn 装载 ARM PIE，任务 ID 来自公共任务身份。重复启动只装载一次，实际程序及线程在共享内核两个 CPU 上执行，退出后保留结果并释放一次真实任务引用，继续检查最后堆回收。这是 ELF/SVC/双端口 RAM RTL 联合证据，不是物理板或 MCU appd 网络服务证据；该 MCU 引导适配器目前仅接入测试配置。

生产普通内核调用、调度器和默认启动项均没有增加应用服务工作。appd 和候选扫描仅在显式启用和调用时消耗资源；按程序数分配不构成性能或全局 CPU 均衡承诺。完整内核、应用、协议报文、日志与源码摘要随提交归档于`.kernel-build/cluster/evidence/app-execution-<commit>/manifest.json`。 Minecraft 的 Java、文件/显示/输入与物理 MCU 验收仍依照[24](24-single-system-minecraft.md)，不会用上述计算程序代替 Minecraft 客户端。

冻结源码的最终验收记录位于 `apps-resource-final/events.json`：CD/1=117、 CD/2=118、CA/1=271，主动丢弃 4 个已接受启动的回复；三个初始客体分别有 4、6、4 次实际工作校验输出，均衡阶段实际活动数为 2/2/2。 MCU 的 `apps-mcu-seed21.json` 为 PASS，两个 CPU 共执行 56,442 条用户指令、 286 次 SVC，报告的 ELF SHA-256 与归档固件一致；该测试的指令交错不代表周期时序或物理板性能。RV64、AArch64、x86 默认服务器 kernel.bin 与前一对象目录提交逐字节一致。公共头跨 ISA 执行、ASan/UBSan、ARMv7-M/RV32 freestanding 编译、集群单元测试、文档及 ABI 检查通过。全仓格式检查仍报告原有 114 个文件，本轮新增/修改文件未增加失败，未修改格式基线。

统一 appd/appctl 入口现已覆盖 Linux ABI 程序，并验证三架构真实程序 2/2/2 分配及完整 Minecraft 客户端菜单、输入、退出；能力、部署与验收边界见[53-linux-application-execution.md](53-linux-application-execution.md)。
