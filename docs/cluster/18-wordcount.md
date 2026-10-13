# 通用字节结果与混合词频作业

实现共用 `cluster_job.h`、`cluster_words.h` 和 `cluster_word_job.h`；MCU leaf 与
Native agent 注册同一算子函数，不维护两个词频实现。协调器持有有界图和结果，
将分片以及中间归约交给能力匹配的节点，自己只封装已提交的字节结果。

## 1. 使用

构建 `make ARCH=riscv64 native-cluster-agent`，按
[17-deployment-supervision.md](17-deployment-supervision.md) 启动包含 agent、
control、jobs 的监管配置。文件系统中的输入文件准备好后，在 Native 环境执行：

```sh
/bin/cluster-wordcount-rv /tmp/input.txt
```

输入 `Alpha beta alpha\n` 时输出：

```text
alpha 2
beta 1
```

CLI 从文件读取至多 1024 B；多一个字节也明确失败。它通过本机 `cluster.jobs`
提交 CJ/1 操作 2、10 s 预算，并核验关联 ID、状态、精确长度和规范词典。
远端服务不可用、作业失败、非法字典、文件读取失败均返回非零退出码。
本地只有一个 agent 也能运行；种子路由与 clusterd 能力目录决定是否使用远端。
空文件成功返回空词典，CLI 不打印词条。

## 2. 算子和编码

WORD_MAP=3 把文本转成词典；单词为连续 ASCII 字母或数字，A–Z 转小写。
其他非零 ASCII 字节是分隔符；NUL 和非 ASCII 输入返回 INVALID。
单词最多 12 B，全局最多 8 个不同词。超长词、词典满或 u32 计数溢出返回
NO_SPACE，不截断、不哈希、不近似计数。

词典显式小端编码：`u32 count`，后跟 count 个 16 B 条目。
每个条目为 12 B 小写字母/数字词名（不足补零）和非零 u32 次数。
词名按完整 12 B 字典序严格升序；空词、重复、乱序、内部零后仍有字符、
大写字母、零计数或多余尾字节均为非法。空词典为 4 B，最大词典 132 B。
12 B 单词允许填满词名字段，不要求额外 NUL。

WORD_MERGE=4 输入一或两个 `[u32 length][dictionary]`，验证每个词典后合并；
输入最大 272 B，输出最多 132 B。CONCAT=5 使用相同长度前缀，拼接任意字节串；
输入最大 344 B，输出最多 168 B，保持前驱顺序，支持空字节串。
两者允许结合归约，但结合律不表示可以交换子节点。

| 节点 | WORD_MAP 输入/输出 | WORD_MERGE 输入/输出 | CONCAT 输入/输出 |
|---|---|---|---|
| 默认 Native agent | 1024/132 B | 272/132 B | 344/168 B |
| 默认 MCU leaf | 140/132 B | 140/132 B | 140/168 B |

这是算子正文能力，不包括 CW 32 B 头。服务还有整体输入/输出、在途和预算限制。
较大的二词典归约不会派给 MCU；较小的归约仍可在 MCU 运行。

## 3. 图与提交接口

词频构图器按至多 76 B 文本切片，只在单词之间切分，最多 16 片、31 个图节点。
正文上限 1024 B；边界处回退到词首，防止把一个词拆成两个。空文本生成一个 MAP。
每片执行 MAP，二叉树中间节点执行 MERGE；奇数节点向上提升。
每个任务持有 132 B 输出，归约封装只引用已提交结果。

CJ WORDCOUNT 沿用 [11-jobs.md](11-jobs.md) 的 32 B 请求头；操作为 2、偏移 16
为文本字节数，32 后为精确原始文本。保留字段必须零，预算 1–30000 ms。
有效请求的回复为 32 B 头 + 词典：偏移 12 为状态，16/20/24 为完成数、重派数、
任务总数，28 为正文长度。失败正文为空。重复 CJ 提交是新作业；jobd 故障没有
持久恢复。原 DOT 操作 1 和固定 40 B 回复保持不变。

通用图可自定义 `combine` 回调与输出缓冲。回调仅封装子结果，必须确定且有界；
不能借回调在协调器重新执行业务归约。共享图最多 64 节点，当前 Native 驱动
接口最多 64 节点。传输重放使用相同 CL 事务；作业重派使用新 CW attempt。
只有当前目标 ID/epoch/attempt 的成功结果可提交一次；旧结果不能改写字节缓冲。

## 4. 可复现验证与边界

```sh
make check-cluster-unit
make check-cluster-fuzz
make smoke-native-cluster
python3 tools/check-cluster-gateway.py --gossip
python3 tools/check-cluster-gateway.py --gossip --abandon
PATH=/path/to/arm-toolchain/bin:$PATH make ARCH=armv7m BOARD=stm32f103 \
  STM32_FLASH_KB=64 STM32_RAM_KB=20 STM32_CLUSTER_PORT=2 kernel-only
```

共享测试遍历 0–1024 B 文本长度，与整段 MAP 的规范结果逐字节比较；覆盖空结果、
CONCAT 顺序、缓冲容量拒绝、旧 attempt、不支持节点、非法字典、溢出。
10 万次词典/归约变异通过 ASan/UBSan。Native smoke 实际启动 jobd 和文件 CLI，
检查 `jobd-word-tree ok`、`jobd-word-cli ok` 与具体词频输出。

网关测试启动两个独立 TCG 内核与生产 MCU leaf 主机进程，路径为
RISC-V→UDP→x86 COM2→UART→leaf；736 B 文本树必须同时使用主机和 leaf。
故意丢弃首个 MCU MAP 回复，最终结果与整段参考一致；MAP 实际执行次数等于
独立 CL 事务数（当前均为 1），没有把同一事务重传重复执行。
日志位于 `.kernel-build/cluster/gateway-gossip/`，记录源文件哈希及注入事件。

该演示是精确、有界 ASCII 词频，不支持 Unicode 分词或任意大词表。
外部数据集流式分片、持久多作业调度与物理板联调仍需单独验证。

`--abandon` 丢弃首个 MCU MAP 事务的所有回复，直到协调器放弃该 attempt。
测试要求至少一次作业重派、至少两次实际丢回复，并在树完成后直接查询 MCU，
2.5 s 内必须成功。放弃收据释放已完成事务的缓存，旧请求仍受去重窗口约束。
主机共享回放测试还覆盖执行中先收放弃通知、不能提前复用预留记录、旧 worker
不能完成新记录、通知先于请求、OOM 后重试和过期旧序号。


## 5. 16 片、8 节点验收

固定数据集为 `tools/fixtures/wordcount.txt`：1014 B、78 个词、8 个不同词，词长
恰为 12 B。76 B 切片在词边界回退，形成 16 片和 31 个实际执行的树节点。
Native 驱动已扩为共享图的 64 节点上限；DOT 仍使用原 15 节点构图器。

```sh
# 八套独立 Native 内核
python3 tools/check-cluster-wordcount.py
# 六套 Native 内核和两个生产 leaf 主机进程；两个 x86 COM2 网关
python3 tools/check-cluster-wordcount.py --mixed
# 顺序执行上述两种配比
make demo-wordcount
```

检查器在启动前将固定数据集复制到镜像，从数据本身用 Python 计数并生成小端
规范参考字典。目标系统从同一文件读取输入：第一棵树先按实际目录能力向每个节点各派一个 MAP，再由 Native 驱动执行剩余
图，保证全部 8 个节点实际执行；轮询 any 本身不承诺均匀覆盖。第二棵树在节点 2 执行 MAP 后丢掉其回复并关闭该内核，要求该分片在别的
节点以新 attempt 完成。两棵树输出都与主机参考逐字节一致，再通过实际 jobd
和文件 CLI 核验排序词频。每个图节点的最终目标/attempt 记录在结果日志中。

混合配比中节点 7/8 使用同一生产 MCU leaf 源码及静态单缓存，通过独立串口
字节流接到节点 5/6 的 COM2。每个 leaf 的实际 MAP 执行数须等于其被接纳的独立 CL
事务数；未接纳且被放弃的请求单独计数；中间较大字典归约按能力选择主机。这是 MCU 运行模型仿真，物理板
持久 Flash、引脚、电气和时序验证仍需另行完成。

产物目录为 `.kernel-build/cluster/wordcount-server/` 与 `wordcount-mixed/`，包含
配置配比、执行目标、故障节点/分片、参考字典、源码哈希、真实线帧及各节点日志。

leaf 来源表固定为 8 项，避免六个主机控制面发现同一 leaf 时挤占只有 4 项的
旧表；每项保留身份、会话和序号窗口，不逐出。实际 64 KiB Flash/20 KiB SRAM
USART2 固件链接后 text=47504 B、data=0、bss=9376 B；比此前 4 项来源表增加
384 B 静态 RAM，单回复缓存仍为 256 B。链接器继续检查线程栈、堆与持久区预算。
