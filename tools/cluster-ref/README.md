# tools/cluster-ref — 线协议参考实现与金样向量（WB1）

主机侧 Python 线协议参考实现 + 金样向量 + C 侧对拍 runner。**不依赖任何内核代码**；
`kernel/cluster/frame.c`（WA）与 MCU 叶子解码器（WC）在进入联调前必须离线通过本目录
的全部向量（`docs/cluster/02-wire-protocol.md` §9 的强制要求）。

规范依据（字段级出处，改动任何常量前先核对）：

| 文件 | 依据 |
|---|---|
| `clframe.py` | `docs/cluster/02-wire-protocol.md` §1–§10 全部常量；`01-abi.md` 节点哈希/errno/限额；`04-transports.md` §3/§4 MTU 与 SLIP；`05-userspace.md` §3 TLV |
| `gen_vectors.py` | 02 §9(1)(2)：合法帧 + 畸形帧 + 期望行为 |
| `gen_scenario.py` | 02 §9(3)：完整事务帧序列 |
| `refdec.c` | 同 `clframe.py`，C 语言第二实现，供对拍与移植参考 |
| `check_c_side.py` | 02 §9 "离线通过全部金样" 的对拍 runner |
| `selftest.py` | 编码→解码往返 + 向量/脚本自洽回归 |

## 快速开始

```bash
cd tools/cluster-ref
python3 selftest.py            # 全部自测（原语/往返/向量再生/脚本复核）
python3 gen_vectors.py         # 重写 vectors/（提交前必须跑，树里不许有漂移）
python3 gen_scenario.py        # 重写 scenario_hello_call_close.txt
python3 check_c_side.py        # 编译 refdec.c 并对拍全部向量
python3 check_c_side.py --decoder ./your_decoder   # 对拍你的内核/叶子解码器
```

`gen_vectors.py` 与 `gen_scenario.py` 的输出是**生成物**：`vectors/` 与
`scenario_hello_call_close.txt` 不许手改；改协议先改 `clframe.py` 与生成器，再重新生成。
`selftest.py` 会把向量整个重生成一遍与提交树逐字节比对，漂移即失败。

## 目录

```
clframe.py                     参考编解码器（唯一权威实现）
  ├ 原语: fnv1a32 / crc16_ccitt (poly 0x1021 init 0xFFFF, CCITT-FALSE)
  ├ 02-1 帧: encode_frame / decode_frame（逐字段 struct.pack，禁止内存映像）
  ├ 02-2/3: 11 种 type、4 个 flag、frag 字段
  ├ 02-6: encode/decode_hello_payload（32 B 定长）
  ├ 02-5: fragment_payload / frag_field / reassemble
  ├ 04-4: slip_encode / slip_decode（END=0xC0 帧首尾各一，DB DC/DB DD）
  ├ 05-3: tlv_encode / tlv_decode（[tag:u16][len:u16][value]，4 B 对齐）
  └ 判决: classify() = decode_frame + validate_dispatch/validate_hello，
         输出 accept/layer/reason/counter/errno/reply 单一权威结论
vectors/                       金样向量（MANIFEST.json 是机器可读清单）
  valid/      02-9(1)  每种 type ≥2 个合法帧：.hex + .json（含逐字段解码结果）
  malformed/  02-9(2)  49 个畸形帧 + 期望行为（丢弃/计数/ERROR+errno）
  reserved/   02-9(2)  保留位非零：02-1 规定"不拒绝、必须忽略"，全部 accept
  state/      02-9(2)  只有带状态接收方才能产生的计数（去重/重组/限额）
  slip/       04-4     SLIP 变体：转义边界、坏转义、半帧、超长、双帧、预算
scenario_hello_call_close.txt  02-9(3) 完整事务（HELLO→CALL→分片→REPLY→CLOSE）
refdec.c                       C 参考解码器（对拍格式的事实标准实现）
check_c_side.py                C 侧对拍 runner
selftest.py                    Python 自测回归
gen_vectors.py / gen_scenario.py  生成器
```

## C 侧对拍 runner 约定（check_c_side.py）

被测解码器是**独立可执行程序**，不链接 Python。约定：

### 调用与退出码

```
<decoder> <file.hex> <mtu>        # 帧模式；mtu = 接收方传输 MTU（见下表）
<decoder> --slip <file.slip.hex>  # SLIP 模式
<decoder> --selftest              # 可选但强烈建议：锚点自检
```

退出码：`0` = 帧接受；`1` = 拒绝（有判决字符串，**属正常结果**，看 `reason=`）；
`2` = 用法错误。malformed 向量以 `reason=` 为准，不看退出码。

### 输出格式（stdout，每行一个 `key=value`，LF 结尾）

强制键：

| 键 | 说明 |
|---|---|
| `wire_len` | 实收字节数 |
| `accept` | `1`/`0` |
| `reason` | 判决字符串，与 `clframe.py` 的 R_*/D_*/H_* 表逐字一致（`OK`、`short_frame`、`bad_magic`、`unsupported_ver`、`unknown_type`、`payload_len_mismatch`、`payload_len_over_mtu`、`unsupported_csum_kind`、`crc_mismatch`、`frag_flag_mismatch`、`seq_on_unreliable_frame`、`txid_on_send`、`fixed_payload_len_mismatch`…完整表见 `clframe.py` 顶部） |

`accept=1` 时还须输出（值格式与向量 JSON 一致，多字节一律大写十六进制）：
`magic=0x4C43` `ver` `type` `type_name` `flags=0x0001` `frag=0x0000` `frag_seq`
`frag_last` `txid` `src_hash=0xXXXXXXXX` `dst_hash` `dst_slot` `seq` `payload_len`
`ttl` `csum_kind` `crc_present` `crc_value=0xXXXX`、`payload_hex=<hex>`。

可选键（存在才比对）：`ack_prefix`/`app_payload_len`/`app_payload_hex`
（RELIABLE 非分片的 SEND/CALL/CALL_REPLY，02-7 捎带剥离）；HELLO/HELLO_ACK 的
`hello_node_id` `hello_node_id_hash` `hello_proto_min` `hello_proto_max`
`hello_profile_tier` `hello_caps` `hello_short_addr` `hello_link_addr_len`
`hello_reserved` `hello_nonce`（02-6 逐字段）。

注意：`clframe.py` 只在 RELIABLE 且非分片时才拆 ACK 前缀（A-05/A-06）；参考实现
refdec 对任何 ≥4 B 的可变载荷都会打印 `ack_prefix` 作为观察值。对拍时以**向量
JSON 的 `payload.ack_prefix` 是否为 null 为准**：null 时不比对可选键。

SLIP 模式键：`stream_len`、`frames=<n>`、`frame0_hex=...`、`notes=<字母串或->`。
字母 ↔ `clframe.slip_decode` 词：`e`=empty（帧首 END 再同步产物）、`u`=
unterminated_escape、`x`=bad_escape、`o`=oversize（超过 512 B 接收缓冲，丢弃到
下一 END）、`p`=partial_frame（流结束于半帧）、`t`=truncated（不足 32 B 头）、
`m`=bad_magic（解转义后首两字节非 `43 4C`）。

### MTU 表（02-1 用**接收方**的传输 MTU 校验 payload_len）

| receiver role | mtu |
|---|---|
| `peer_udp` `head_udp` `peer_udp_hello` `head_udp_hello` | 1472（04-3） |
| `head_uart` `head_uart_hello` `leaf_uart` `leaf_uart_down` | 256（04-4） |
| `loopback` | 65536（04-5） |

向量 JSON 的 `receiver.role` 给出角色；`transport.mtu` 是**发送侧**的名义传输，
二者可以不同（例：`state-mcu-oversize-01` 的帧按 UDP MTU 解码成立，MCU 档在
发送侧拒绝——这正是该向量要钉住的行为）。

### 对拍内容

每个向量：`accept`、`reason`、`wire_len`；帧可读时全部头部字段 + `payload_hex`；
JSON 有 `ack_prefix` 时比对捎带拆分；HELLO 向量比对 02-6 十个字段；SLIP 向量比对
帧数、每帧 hex、notes 序列。`--print <vid>` 可看单个向量的解码输出。

## 覆盖清单 ↔ 02-wire-protocol.md §9

| §9 要求 | 落实 | 数量 |
|---|---|---|
| (1) 每种 type ≥2 个合法帧 hex + JSON 解码结果 | `vectors/valid/`，MANIFEST `counts.valid_by_type` 全 type ≥2 | 30 |
| (2) ≥20 畸形帧（错 magic、payload_len 越界、保留位非零、frag 跳号、CRC 错、ver 过高）+ 期望行为 | `vectors/malformed/`（49）+ `reserved/`（7，保留位**接受**分支）+ `state/`（6，frag 跳号=重组态见下）；每个 JSON 的 `expect.dispatch` 给出 action/reason/counters/errno/reply | 62 |
| (3) 完整事务帧序列（HELLO→CALL→分片→REPLY→CLOSE） | `scenario_hello_call_close.txt` 17 步，含 PING/PONG、裸 ACK、重传、坏帧、重组超时、ERROR、UART/SLIP 附录 | — |

§10 十个计数器中 7 个由单向量钉住（MANIFEST `counts.counters_exercised`）；
`tx_frames`/`retransmits`/`reasm_timeouts` 需要帧序列，由 scenario 文件的计数
小结钉住。

## 假设登记簿（ASSUMPTION A-nn）

02 文档留白处、实现必须二选一的点，全部在此登记并在代码里就地标注。改判定先改这里。

| # | 内容 | 依据与备选读法 |
|---|---|---|
| A-01 | 多字节字段小端 | 02-1 明文；wire 上 magic = `43 4C`（"CL"）。`mal-magic-swapped-02` 钉住 |
| A-02 | CRC 覆盖 32 B 头 + 载荷，LE 附于尾 | 02-1 只说"附于载荷尾 2 字节"。`mal-crc-flip-header-03`（翻 ttl 位必须被抓住）钉住头也参与；备选读法（只覆盖载荷）会被该向量判死 |
| A-03 | `payload_len` 只数载荷，不含头与 CRC | 02-1 "载荷字节数"；由此 `len == 32 + payload_len + crc_len` 可校验，`mal-len-trailing-01`/`mal-len-crc-missing-01` 钉住 |
| A-04 | 单帧装得下的消息不置 FRAGMENTED：`frag=0 且 last=1` 判 `frag_flag_mismatch` | 02-5 "序号从 0 递增、末片 bit15"；备选读法（允许单片）与"递增"矛盾。`mal-frag-single-03` |
| A-05 | 分片内不捎带 ACK 前缀，片载荷恰为 MTU−32−2 | 02-5 明文片大小 1438，无处安放前缀。`fragment_payload(ack_prefix_len≠0)` 抛错 |
| A-05b | HELLO 载荷内多字节整数小端 | 02-1 的小端规则未重复于载荷；05-3 "整数小端" 是唯一旁证 |
| A-06 | ACK 捎带只出现在非分片 RELIABLE 的 SEND/CALL/CALL_REPLY | 02-7 "任何反向帧" 会与 02-2 定长载荷列（ACK 本身 4 B）矛盾 |
| A-07 | TLV 对齐按**每个元素**：value 后 pad `(-len)%4`，pad 不计入 len | 05-3 "对齐到 4 字节" 两种读法字节不同；备选（整条尾部对齐）未采纳 |
| A-08 | ERROR 帧合法 errno 集 = 01-abi 列为 connect/call 结果的 5 个码（24/26/27/28/29） | 02-8 "只用 01-abi 定义的集群 errno" 与 02-2 要求 NOT_FOUND 矛盾，取并集；未知码映射 CLUSTER_UNSUPPORTED |
| A-09 | `payload_len ≤ MTU−32−2` 无条件减 2，含 csum_kind=0 | 02-1 原文字面；备选读法（无 CRC 时 +2）未采纳 |
| A-10 | 帧首 END 兼作再同步点：连续 END 之间为空帧，无帧、无计数 | 04-4 明文"帧首 END 充当再同步点"。`slip-mal-resync-01` |
| A-11 | 只有置 RELIABLE 的帧消耗 seq；裸 ACK/NACK/控制帧 seq=0 | 02-1 "不可靠档置 0" + 02-7 seq per link；备选（控制帧也占 seq）与 valid-ack-01 冲突 |
| A-12 | 02-10 计数器口径：线格式违规=`rx_malformed`；格式对但策略/状态拒收=`rx_drops`；seq 重复=`dedup_drops`；HELLO 协商失败=`hello_rejects` | 02-10 只给名字不给口径；此划分使每个向量可判定 |
| A-13 | TTL 递减属于转发跳：到达帧 ttl=0 才算耗尽（ttl=1 仍投递） | 02-1 "每跳 -1，到 0 丢弃"；`valid-pong-02`（ttl=1 接受）+ `mal-ttl-zero-01` |
| A-14 | 自环 HELLO（自己的 nonce）静默丢弃，不回 ERROR（回给自己无意义） | 02-6 "判定为自环，拒绝"；备选（回 ERROR）未采纳。`mal-hello-self-loop-05` |
| A-15 | 02-1 的"其余置 0"列（SEND 的 txid 等）是**发送侧**规则；接收侧对 SEND 的 txid≠0 拒收（`rx_malformed`），对 CLOSE/PING 等的冗余字段忽略不拒 | `mal-send-txid-01`（拒）与 `res-txid-on-close-06`/`res-dst-slot-on-ping-05`（忽略）分别钉住两侧 |
| A-16 | COMPRESSED（02-3 "v0 保留必须 0"）按 02-1 保留位规则**接受并忽略** | 02-1 "对非零保留位不拒绝" 优先于 02-3 的"必须 0"；备选（拒收）未采纳 |
| A-17 | UART 320 B 发送缓冲与 04-4 "叶子算子载荷 ≤128 B" 自洽：最坏转义 2×(32+p)+4，p=129 恰 320 | `slip-uart-budget-06`（全 0xDB 载荷 128 B）实测 320 B 内 |

**frag 跳号（02-9(2) 字面）与 02-5 "允许乱序到达" 的矛盾**：缺片不是线格式错误。
落实为 `state-reasm-gap-01`（收下、入位图、5 s 后 `reasm_timeouts`），
`mal-frag-*` 三个向量钉真正的 frag 字段违规（无 flag、last 无 flag、单片）。
已在 02 §9 回写（见下"规范回写"）。

## 规范回写（实现期偏差，核对日期 2026-10）

以下偏差/澄清已回写进规范文档（含日期标注），此处为指针：

1. `02-wire-protocol.md` §9 末尾新增"实现期核对"段：本登记簿是 §9 全部金样的
   判定细则来源；frag 跳号按 02-5 乱序语义落在重组态（`state-reasm-gap-01`），
   frag 字段违规三例在 `mal-frag-*`。
2. `04-transports.md` §4 新增一段：SLIP 层 finish 判定（`t`/`m`：不足 32 B 头、
   解转义后坏 magic 的"帧"不交给 CL 层）与 oversize 丢弃-到-下一 END 语义。
3. `05-userspace.md` §3 新增一句：TLV 对齐按元素（A-07），pad 不计入 len。

## 判决速查（clframe 常量 ↔ 语义 ↔ 计数）

层 1（`decode_frame`，纯函数）：`short_frame`/`bad_magic`/`unsupported_ver`/
`unknown_type`/`unsupported_csum_kind`/`payload_len_over_mtu`/
`payload_len_mismatch`/`crc_mismatch`/`frag_flag_mismatch`/
`seq_on_unreliable_frame`/`txid_on_send`/`fixed_payload_len_mismatch`
→ 一律 `rx_malformed`。
层 2（`validate_dispatch`，需对端状态）：`dst_is_local`/`broadcast_forbidden_on_profile`/
`broadcast_flag_on_non_send`/`reliable_without_negotiated_cap`/
`fragment_forbidden_on_profile`/`ttl_expired`（→ 本地 errno
`A20_ERR_NODE_UNREACHABLE`）/`duplicate_seq`（`dedup_drops`）/
`unexpected_type_for_state` → `rx_drops`。
层 2（`validate_hello`）：`hello_proto_not_negotiable`/`hello_hash_mismatch`/
`hello_hash_clash`/`hello_nonce_self_loop`/`hello_reserved_node_id`/
`hello_caps_not_legal`/`hello_relay_on_leaf` → `hello_rejects` + ERROR
（自环除外，A-14）。
ERROR 载荷层：`error_errno_unknown`（按 02-8 映射 CLUSTER_UNSUPPORTED，
帧仍投递）/`error_orig_txid_not_inflight`（`rx_drops`，02-4 迟到应答规则）。
