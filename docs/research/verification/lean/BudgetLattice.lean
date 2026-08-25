/-!
# Capability Envelope -- Budget Lattice (C4-Lean)

Budget monotone decay formalization from docs/research/03 Thm 3.1/3.2.
Core Lean 4 only; no external dependencies.
-/

/-- Deducting `n` from budget `b` yields `b - n` (truncated at zero). -/
def deduct (b n : Nat) : Nat := b - n

/-- THEOREM (03 Thm 3.1): deduction never increases the budget. -/
theorem deduct_le (b n : Nat) : deduct b n ≤ b :=
  Nat.sub_le b n

/-- THEOREM (03 Thm 3.2): strict decay when both positive. -/
theorem deduct_strict (b n : Nat) (hb : b > 0) (hn : n > 0) :
    deduct b n < b := by
  unfold deduct
  omega

/-- COROLLARY: transitive decay through chained spending. -/
theorem decay_transitive (ceiling mid final : Nat)
    (h₁ : mid ≤ ceiling) (h₂ : final ≤ mid) :
    final ≤ ceiling :=
  Nat.le_trans h₂ h₁

/-- Expiry latch: once true, stays true. -/
theorem latch_true (prev tickPast : Bool) (h : prev = true) :
    prev || tickPast = true := by rw [h]; trivial

/-! ### Summary of checked properties

| Property | Source | Status |
|----------|--------|--------|
| `deduct_le`         | 03 Thm 3.1 monotone decay     | ✅ proved |
| `deduct_strict`     | 03 Thm 3.2 strict decay       | ✅ proved |
| `decay_transitive`  | 03 §3.2 transitive chain      | ✅ proved |
| `latch_true`        | 05 §2.5.2 one-way latch       | ✅ proved |
-/

/-! ## Rights model (kernel: uint64_t bitmask as individual Bool fields) -/

/-- Capability rights as independent Boolean fields matching kernel
A20_RIGHT_READ/WRITE/STAT/SEEK bits. -/
structure Rights where
  rd : Bool
  wr : Bool
  st : Bool
  sk : Bool

/-! ## Extended properties: transfer clamping & direction enforcement -/

/-- Per-field Boolean AND clamp (kernel: uint64_t AND semantics). -/
def andField (req cap : Bool) : Bool := req && cap

/-- THEOREM: clamped result never exceeds the cap.
If the cap is false, the clamped result is false regardless of request. -/
theorem and_field_le_cap (req cap : Bool) :
    (andField req cap) = true → cap = true := by
  unfold andField
  intro h
  cases req <;> cases cap <;> simp at h ⊢

/-- THEOREM: if both request and cap are true, the clamped result is true.
This is the positive direction: legitimate authority survives clamping. -/
theorem and_field_preserves (req cap : Bool)
    (h₁ : req = true) (h₂ : cap = true) :
    (andField req cap) = true := by
  unfold andField
  rw [h₁, h₂]
  trivial

/-- THEOREM: clamp idempotence -- applying the clamp twice yields the
same result as applying it once (repeated mediation cannot expand rights). -/
theorem and_field_idem (req cap : Bool) :
    andField (andField req cap) cap = andField req cap := by
  cases req <;> cases cap <;> rfl

/-! ### Transfer clamping applied per-right -/

/-- Transfer clamp: grant = request ∩ cap (per-field). -/
structure ClampedRights where
  rdClamped : Bool
  wrClamped : Bool
  stClamped : Bool
  skClamped : Bool

def xferGrant (req cap : Rights) : ClampedRights where
  rdClamped := andField req.rd cap.rd
  wrClamped := andField req.wr cap.wr
  stClamped := andField req.st cap.st
  skClamped := andField req.sk cap.sk

/-! ### Direction enforcement through transferred authorities

After a clamped transfer installs a shadow, subsequent WRITE-direction
operations require the write bit in the clamped rights set. -/

/-- Write-direction pass on transferred authority requires BOTH source
request AND policy cap granted the write bit. -/
theorem xferred_wr_needs_both (req cap : Rights)
    (h_grant : (xferGrant req cap).wrClamped = true) :
    req.wr = true ∧ cap.wr = true := by
  unfold xferGrant at h_grant
  simp [andField] at h_grant
  exact h_grant

/-- Read-direction analogue: read-pass implies both granted read. -/
theorem xferred_rd_needs_both (req cap : Rights)
    (h_grant : (xferGrant req cap).rdClamped = true) :
    req.rd = true ∧ cap.rd = true := by
  unfold xferGrant at h_grant
  simp [andField] at h_grant
  exact h_grant

/-! ## Cross-envelope budget refinement (05 §2.5.4 SCM_RIGHTS receive)

When a capability travels over SCM_RIGHTS, the receiver's budget is
charged min(sender_charge, receiver_remaining).  Two bounds hold:

1. Receiver never pays more than its own remaining budget.
2. Receiver never pays more than the sender-side charge.

Together with del_wr/rd_needs_both this closes BOTH dimensions of the
transfer story: rights cannot escalate (above), budget cannot overdraw
(here).  These theorems are SAFETY SPECIFICATIONS for receive-time
charging, not descriptions of v1 internals: the v1 kernel satisfies
both bounds by construction -- env_mediate_acquire_gfd charges the
receiver exactly one op drawn from its own remaining_ops (denied at 0)
and couples to no sender-side amount. -/

/-- Specification of the charge applied at SCM_RIGHTS/pidfd_getfd
receive time: any conforming path charges at most
min(sender-side amount, receiver remaining). -/
def scmRecvCharge (senderCharge recvRemain : Nat) : Nat :=
  min senderCharge recvRemain

/-- THEOREM: incoming transfer never charges beyond the receiver's
remaining budget -- the envelope cannot be driven negative by a gift. -/
theorem scm_recv_le_receiver (s r : Nat) :
    scmRecvCharge s r ≤ r := Nat.min_le_right _ _

/-- THEOREM: incoming transfer never charges beyond the sender-side
amount -- a generous sender cannot drain the receiver via transfer. -/
theorem scm_recv_le_sender (s r : Nat) :
    scmRecvCharge s r ≤ s := Nat.min_le_left _ _

/-! ### Summary of extended checked properties

| Property | Source | Status |
|----------|--------|--------|
| `and_field_le_cap`       | A7 clamp ≤ cap              | ✅ proved |
| `and_field_preserves`    | A7 clamp preserves legit    | ✅ proved |
| `and_field_idem`         | A6/A7 clamp idempotence     | ✅ proved |
| `xferred_wr_needs_both`  | A6 wr dual-grant requirement| ✅ proved |
| `xferred_rd_needs_both`  | A6 rd dual-grant requirement| ✅ proved |

### Delegation bounding (03 §3.2)

| Property | Source | Status |
|----------|--------|--------|
| `del_wr_needs_both`      | 03 §3.2 wr no-escalation    | ✅ proved |
| `del_rd_needs_both`      | 03 §3.2 rd no-escalation    | ✅ proved |

### Cross-envelope budget refinement (05 §2.5.4)

| Property | Source | Status |
|----------|--------|--------|
| `scm_recv_le_receiver`   | 05 §2.5.4 recv ≤ own remain | ✅ proved |
| `scm_recv_le_sender`     | 05 §2.5.4 recv ≤ sender amt | ✅ proved |
-/

/-! ## Delegation rights bounding (03 §3.2 rights monotone decrease)

When a capability is delegated A→B, effective rights are computed as
A.rights ∩ B.cap (per-field AND).  Any right exercised through the
delegated capability required BOTH the delegator's grant AND the
receiver's cap -- no upgrade path exists through the trust chain.
Proof strategy mirrors xferred_*_needs_both above. -/

/-- Delegation result: per-field AND of delegator rights and receiver cap.
Mirrors kernel clampRights used during transfer mediation. -/
def delegateRights (dr rc : Rights) : Rights :=
  { rd := dr.rd && rc.rd
    wr := dr.wr && rc.wr
    st := dr.st && rc.st
    sk := dr.sk && rc.sk }

/-- THEOREM: delegated write exercised ⇒ delegator granted write AND
receiver cap granted write. Delegation cannot create authority beyond
either party's limits (03 §3.2 no-escalation). -/
theorem del_wr_needs_both (dr rc : Rights)
    (h : (delegateRights dr rc).wr = true) :
    dr.wr = true ∧ rc.wr = true := by
  unfold delegateRights at h
  simp at h
  exact h

/-- THEOREM: read-direction analogue. -/
theorem del_rd_needs_both (dr rc : Rights)
    (h : (delegateRights dr rc).rd = true) :
    dr.rd = true ∧ rc.rd = true := by
  unfold delegateRights at h
  simp at h
  exact h
