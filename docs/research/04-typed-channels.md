# 类型化通道（Typed Channels）——配套贡献

> **本文是预算能力（03）的"传播维度"**：在预算格（类型 × 权限 × 时间 × 次数 × 传播）中，typed channel 实现的是**传播维度**——能力能流向谁、流什么类型。它与时间/次数维度（03）组合构成供应链防御（03 定理 7.1），并支撑 05 信封的"可向外传播的类型"策略。在论文中作为 C1 的组成部分与信封的支撑材料，不单独主张贡献。 基础 SOS 模型见 06。

---

## 1. 问题重述（预算格的传播维度）

在预算能力模型（03）中，能力是"能碰到哪类、能多久、多少次、能流向谁"的多维预算。本维度回答"**能力能流向谁**"：主流能力系统的 IPC 传播路径无类型约束——
- Zircon channel 是无类型字节流；FIDL 类型检查在**用户态**解码器完成，内核不阻止错误类型的 handle 进入错误通道。
- seL4 endpoint 无类型约束；类型化组件框架在 seL4 之上用户态实现。

**后果**：能力流图在运行前不可静态判定；confused deputy（Hardy 1988）与 IPC 类型混淆只能靠用户态约定防御；"这个组件到底能把哪些类型的 handle 传给谁"在内核层面没有答案。

---

## 2. 设计

### 2.1 通道类型签名

```c
typedef struct a20_channel_type {
    uint32_t version;            // 结构体版本化（E-APPEND 规则）
    uint32_t send_handle_types;  // bitmask: 可发送的 handle 类型
    uint32_t recv_handle_types;  // bitmask: 可接收的 handle 类型
    uint32_t max_data_size;      // 最大字节负载
    uint32_t max_handles;        // 单条消息最大 handle 数
    uint32_t flags;              // ORDERED / STRICT
} a20_channel_type_t;
```

类型 bit 与 `a20_object_type_t` 对齐（FILE/SOCKET/CHANNEL/PIPE/EVENTQ/TIMER/MEMORY/TASK/NAMESPACE...）。`channel_create(type=NULL)` 保持无类型约束向后兼容。

### 2.2 内核强制点

- **send**：每个被传输 handle 的对象类型必须 ∈ `T.send_handle_types`；`|data| ≤ max_data_size`；`|handles| ≤ max_handles`。违例 → `TYPE_MISMATCH`。
- **recv**：消息中每个 handle 类型必须 ∈ `T.recv_handle_types`。
- 类型签名在 `channel_create` 时复制到两个端点，之后只读（无修改 syscall）——这是"能力防火墙"完整性的实现基础。

---

## 3. 形式化

### 3.1 SOS 扩展

**CH-TYPED-SEND**（在 06 CH-SEND 基础上增加三条前提）：

$$\frac{HT_p(e) = (c, \{W\}) \quad type(c) = T \quad \forall h_i \in handles.\ HT_p(h_i) = (o_i, \rho_i) \land Transfer \in \rho_i \land \tau(o_i) \in T.send\_handle\_types \quad |data| \leq T.max\_data\_size \quad |handles| \leq T.max\_handles}{\langle send_p(e, data, handles), \sigma \rangle \longrightarrow \langle ok(0), \sigma[queue(c') \mathrel{+}= msg] \rangle}$$

**CH-TYPED-SEND-ERR**：任一类型不在 `T.send_handle_types` → `TYPE_MISMATCH`，消息不排队。CH-RECV 对称地检查 `T.recv_handle_types`。

### 3.2 定理

**定理 2.1（通道类型安全）** 对类型 $T$ 的 channel $c$，$c$ 上传输过的所有消息的 handle 都满足类型约束：

$$\forall m \in messages(c).\ \forall h \in m.handles.\ \tau(o_h) \in T.send\_handle\_types$$

*证明*：对 $c$ 生命期内所有 send 归纳。成功 send 的前提含类型检查；失败 send 不排队。$\square$

**定理 2.2（类型化能力流不变式）** 若 $p_1$ 到 $p_2$ 的所有 channel 类型都不允许对象类型 $\tau$，则 $p_1$ 无法经这些 channel 把类型 $\tau$ 的 handle 传给 $p_2$。

*证明*：由 CH-TYPED-SEND 前提，传输需 $\tau \in T.send\_handle\_types$；假设矛盾。$\square$

**推论 2.2.1（能力流静态可判定）** 给定能力流图 $G_{cf}$（顶点=进程，边=类型化 channel）与对象类型 $\tau$，可**在运行前**静态确定哪些进程对可传输类型 $\tau$ 的 handle。

**定理 2.3（协议合规性，可选 ORDERED 模式）** 若 `A20_CHAN_TYPE_ORDERED` 置位，channel 维护步骤指针 $step(c)$，成功操作序列严格遵循声明的步骤序列。

**定理 8.3（confused deputy 静态消除，重述）** 若 deputy $D$ 的入通道 $T_1.recv \cap$ 出通道 $T_2.send = \emptyset$，则攻击者无法经 $D$ 把任何 handle 传给资源方。该条件**运行前可静态检查**（仅验证类型交集）。

---

## 4. 实现状态

- [x] `channel_create` 复制用户类型签名到两端点；send 强制 `send_handle_types`/`max_data_size`/`max_handles`；recv 强制 `recv_handle_types`。`kernel/ipc/a20_channel.c`
- [x] `user/tests/test_native_handle.c`、`test_native_ipc.c` 覆盖 typed create、类型违例返回 `TYPE_MISMATCH`、与无类型通道的向后兼容。
- [ ] 推论 2.2.1 的静态能力流分析**工具未实现**——当前只有语义保证，没有分析器。
- [ ] 定理 2.1/2.2/8.3 未机器检验（08 模块 2）。

---

## 5. 威胁与风险

1. **优先性**：L4 typed IPC、OCaml/ML 系内核、嵌入式组件系统（TOR/Genode）是否有内核层类型约束 IPC 先例——09 §3.2 的 `[待核]` 条目。
2. **实用性**：静态分析工具（推论 2.2.1）不做，卖点不完整。需要实现能力流分析器并在评估（10 E7）中演示。
3. **向后兼容**：`type=NULL` 的宽松路径会削弱"全系统类型纪律"叙事——论文应主张"渐进启用"，与 05 的增量部署一致。
