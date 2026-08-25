# Capability Envelope Research Status

> Consolidated reference for all artifacts, verification results, and
> remaining work items on branch `research/osdi-envelopes`.
> Last updated: 2026-08-25.

---

## 1. Verified Deliverables

### 1.1 Kernel Mediator (`kernel/ipc/envelope.c`, ~670 lines)

| Component | Status | Notes |
|-----------|--------|-------|
| Policy snapshot + shadow handle table | ✅ implemented | Keyed by global fd |
| Acquisition mediation (A1–A3) | ✅ hooked | openat/socket/pipe/memfd/eventfd/timerfd/signalfd |
| Class-only mediation (A5) | ✅ hooked | shmat MEMORY class check |
| Transfer clamp (A6/A7) | ✅ hooked | SCM_RIGHTS recv/send + pidfd_getfd |
| Direction enforcement | ✅ proved | Per-field Bool AND semantics |
| io_uring execution-point + REGISTER_FILES fail-closed (A9/A10) | ✅ hooked | SQE execution-point charge |
| Lazy expiry sweep + KILL_ON_EXPIRE | ✅ implemented | One-way latch |
| Active revocation | ✅ implemented | Owner/root gated |
| Control-plane syscalls 902–906 | ✅ wired | create/enter/revoke/stats/audit |
| USE-side hooks: bind/connect/listen | ✅ added | Ops charge |
| readv/writev byte-granularity charging | ✅ fixed | Direction-aware pre-charge |

### 1.2 Attack Suite (`user/cmds/core/envelope_smoke.c`, QEMU verified)

| Scenario | Result |
|----------|--------|
| Functional (benign file ops inside envelope) | ✅ PASS |
| Type denial (socket outside allowed\_types → EPERM) | ✅ PASS |
| Rights denial (O\_WRONLY vs RO cap → EACCES) | ✅ PASS |
| Op-budget exhaustion (third op after budget=2) | ✅ PASS |
| Data-budget pre-charge (over-budget write denied) | ✅ PASS |
| Temporal expiry (post-deadline read denied) | ✅ PASS |
| Revoke+kill (active revocation → SIGKILL worker) | ✅ PASS |
| Reopen upgrade (/proc/self/fd write-after-RO) | ✅ PASS |
| SCM receive (in-fd mediated; out-of-class dropped) | ✅ PASS |
| SCM receive deny (class outside allowed → no cmsg) | ✅ PASS |
| SCM send propagation (propagation\_types gate) | ✅ PASS |
| pidfd-getfd (cross-task theft per class) | ✅ PASS |
| shmat class (MEMORY outside allowed → EPERM) | ✅ PASS |
| Runtime audit (zero violations post-exercise) | ✅ PASS |

Gate: `make smoke-envelope`

### 1.3 Pilot Experiment (`user/cmds/core/envelope_pilot.c`, QEMU verified)

Five scenarios × four defense arms = **20 cells**, all matching expected
outcomes (docs/research/10-evaluation.md §4.1).

Key finding quantified: LL-permissive admits all attacks; LL-strict
kills benign installs; **envelope holds both dimensions simultaneously**.

Additional real-binary cells:
- G1 mksh-flow: execve-preserved mediation proven on real static binary
- G2 wget-blocked: network authority denied on real binary (SOCKET ∉ allowed)
- G3 shmat-deny: MEMORY class enforcement confirmed
- G4 pipe-flow: multi-process pipeline under ONE shared envelope ——
  pipe2 获取 ×2（A3 PIPE_ENDPOINT R|W|STAT）、每段 fork 继承（05 §2.3 v1
  shared root）、execve 保持、末端重定向 openat+write USE 计费，三进程全
  被中介；纯 builtin 实现，零外部二进制依赖

Gate: `make smoke-envelope-pilot`

E8 盘点备注：预构建 extra.img（256 MB ext4，176 个静态 musl 二进制，
含 mksh 与 coreutils 全套）**不含 git/vim**——staging 目录从未填充，
包获取在本环境被网络限制阻塞。真实负载深化因此以多进程 shell 管道
（G4）推进，而非重型工具接入。

### 1.4 Overhead Microbenchmark (`user/cmds/core/envelope_bench.c`, QEMU directional)

| Operation | Off (ns/op) | On (ns/op) | Δ |
|-----------|------------|-----------|---|
| open+close (A1 acquire) | 79,587 | 82,834 | **+4.1%** |
| read 64B (direction R) | 12,238 | 15,815 | +29.2% |
| write 64B (direction W) | 32,049 | 37,731 | +17.7% |
| lseek control (unmediated) | 11,383 | 11,507 | +1.1% noise |
| socket sendto+drain 64B（AF_UNIX 对，含排空） | 96,051 | 104,795 | +9.1% |

Gate: `make smoke-envelope-bench`
数字即最终报告数字：固定 QEMU/TCG 配置、公开可复现；安全与性能结论均不依赖特定宿主硬件。

---

## 2. Formal Verification Artifacts

### 2.1 TLA+ Model Check (`Envelope.tla`, 214 lines, TLC clean)

| Invariant / Property | Result |
|---------------------|--------|
| TypeAllowed | ✅ holds |
| RightsSubCap | ✅ holds |
| OpsNonNeg / DataNonNeg | ✅ holds |
| ClockBounded | ✅ holds |
| DenyAfterExpiry（过期 ⇒ 全部受介操作禁用） | ✅ holds |
| NoResurrect (temporal) | ✅ holds |
| KillOnce (temporal) | ✅ holds |
| ExpiryLiveness `<>expired` (fairness-liveness) | ✅ holds |
| KillLiveness `expired ~> killSent` (fairness-liveness) | ✅ holds |

Fairness assumptions: WF_vars(Tick) ∧ WF_vars(LazyExpire) ∧ WF_vars(Kill)
——对应内核 sweeper 行为（syscall 入口惰性清扫 + 时钟推进 + KILL_ON_EXPIRE）。

TLC stats: 27,829 generated / 4,620 distinct states / depth 14 / <1s.
Reproduce: `java -cp tla2tools.jar tlc2.TLC -cleanup Envelope.tla`

### 2.2 Lean 4 Proofs (`BudgetLattice.lean`, zero sorry, v4.33.1)

| Theorem | Source | Status |
|---------|--------|--------|
| deduct_le | 03 Thm 3.1 monotone decay | ✅ proved |
| deduct_strict | 03 Thm 3.2 strict decay | ✅ proved |
| decay_transitive | 03 §3.2 chained spending | ✅ proved |
| latch_true | 05 §2.5.2 one-way latch | ✅ proved |
| and_field_le_cap | A7 clamp ≤ policy cap（不超出 cap） | ✅ proved |
| and_field_preserves | A7 clamp preserves legit authority | ✅ proved |
| and_field_idem | A6/A7 clamp idempotence | ✅ proved |
| xferred_wr_needs_both | A6 write dual-grant requirement | ✅ proved |
| xferred_rd_needs_both | A6 read dual-grant requirement | ✅ proved |

Delegation bounding（委托无升级，03 §3.2 rights monotone decrease）：

| Theorem | Source | Status |
|---------|--------|--------|
| del_wr_needs_both | 委托权行使 ⇒ 委托方授权 ∧ 接收方 cap 双真 | ✅ proved |
| del_rd_needs_both | 读方向 analogue | ✅ proved |

Cross-envelope budget refinement（跨信封预算细化，05 §2.5.4）：

| Theorem | Source | Status |
|---------|--------|--------|
| scm_recv_le_receiver | 接收计费 ≤ 接收方剩余（安全规格界；v1 构造性满足：接收仅扣自身 1 op） | ✅ proved |
| scm_recv_le_sender | 接收计费 ≤ 发送方侧金额（v1 无发送方耦合扣费，平凡满足） | ✅ proved |

Reproduce: `lean BudgetLattice.lean` (Lean v4.33.1 core, no Mathlib)

### 2.3 Coverage Matrix (`envelope_coverage.md`, mechanically checked)

366 syscall table entries classified:
- **ACQUIRE**: 10 (openat/socket/pipe2/memfd/eventfd/timerfd/signalfd/shmat/accept4/accept)
- **TRANSFER**: 3 (sendmsg/recvmsg SCM_RIGHTS 分支/pidfd_getfd)
- **USE**: 15 (fs read 家族 6 + io_uring exec + socket bind/connect/listen + 数据面 sendto/recvfrom/sendmmsg/recvmmsg/recvmmsg_time64)
- **FAILCLOSED**: 2 (io_uring_setup/io_uring_register)
- **PLANNED-W2**: 36 (enumerated, tracked)
- **NA**: 300 (no resource authority)

Gate: `make check-envelope-coverage`

---

## 3. Documentation Updates

| File | Changes |
|------|---------|
| 00-index.md | Status sync: C2 mediator implemented, gates green |
| 05-capability-envelopes.md | §2.5 escape-surface design added; §7 checklist updated |
| 08-verification.md | §2.4 TLA+/Lean results recorded; runtime audit noted |
| 09-literature-review.md | Latch/Leash/Decap added; io_uring escape surfaces; revocation lineage |
| 10-evaluation.md | E2 pilot [实测完成]; E11 [实测-QEMU 初步] recorded |
| 12-positioning.md | C2/C4 status cells updated |
| testing-gates.md | All four envelope gates registered with run instructions |

---

## 4. Remaining Queue

| Item | Priority | Blocker |
|------|----------|---------|
| E8 git/vim real-workload flows | HIGH | Requires extra.img infrastructure investigation |
| C4-Lean advanced lemmas (composition, fairness) | MEDIUM | Toolchain ready (v4.33.1); requires careful mathematical work |
| W2-low remainder (shm dirty-page, FD_TIE_LIFETIME strong mode, SCM min(sender)) | LOW | Design decisions needed before coding |

---

## 5. Reproduction Commands

```bash
# Attack suite (QEMU, ~60s)
make smoke-envelope

# Pilot matrix (QEMU, ~120s)
make smoke-envelope-pilot

# Overhead benchmark (QEMU, ~180s)
make smoke-envelope-bench

# Coverage matrix drift gate (instant)
make check-envelope-coverage

# TLA+ model check (<1s)
java -cp tla2tools.jar tlc2.TLC -cleanup Envelope.tla

# Lean proofs (<1s)
lean BudgetLattice.lean
```

All commands run from repo root with riscv64 cross-toolchain available.
