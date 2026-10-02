# LeafLock TLA+ Model — Coverage

Narrowed formal artifact for the page-table node lock handoff
(`kernel/mm/pt.c`: `mcs_lock()` / `mcs_unlock()`), on branch
`feat/mm-single-level`. Companion to `LeafLock.tla` / `LeafLock.cfg`.

## Why this protocol

It shipped with two independent fatal bugs, both in the handoff:

1. **The waiter never published itself to its predecessor's `->next`.** Every
   unlock therefore read `me->next == 0` and took the compare-exchange branch —
   but `m->lock` had already been moved to the *waiter* by the exchange, so that
   CAS always failed. Nothing was released and the waiter was never woken. One
   contended acquisition wedged the node permanently.
2. **The unlocker recovered its own node from `m->node`**, a shared field every
   acquirer overwrote, so a holder with a waiter behind it read back the waiter
   and handed the lock to nobody.

Neither was reported: the waiter is not self-deadlocked, it waits for a handoff
the protocol cannot produce, so `[MCS DEADLOCK]` stays silent. Neither review nor
any pre-Phase-3 workload reached them, because `mm->lock` kept node-lock
contention at zero.

## Checked properties (`LeafLock.cfg`, CPUS = {1,2,3})

| Invariant / Property | Meaning | Result |
|---|---|---|
| `TypeOK` | `lock`/`state`/`succ` well-typed | ✅ holds |
| `MutualExclusion` | ≤ 1 CPU in the critical section per node | ✅ holds |
| `WaiterLinked` | every waiter is some CPU's successor | ✅ holds |
| `FreedHasNoWaiter` | a freed node has nobody waiting on it | ✅ holds |
| `SuccIsWaiter` | successor edges are never stale | ✅ holds |
| `WaiterProgress` (liveness) | a waiter is eventually handed the lock | ✅ holds |

Fairness: `WF_vars(Enter(c))` ∧ `WF_vars(Exit(c))` for all `c` — a scheduler
that eventually runs every runnable CPU.

TLC 2.16: **43 states generated / 16 distinct / depth 4**, no error.

## Teeth test — the model must fail on the real bugs

A passing model is worthless unless it rejects the defects it exists to catch.
Both bugs were reintroduced as single-conjunct mutants and TLC run again:

| Mutant | Change | Expected failure | Result |
|---|---|---|---|
| Bug 1 | drop `succ' = [succ EXCEPT ![lock] = c]` from contended `Enter` | `WaiterLinked` | ✅ **violated** (5 states) |
| Bug 2 | `Exit` always frees the lock, dropping any successor | `FreedHasNoWaiter` | ✅ **violated** (16 states) |

A third mutant was tried and **discarded as invalid**: making `Exit`'s tail
branch `UNCHANGED succ` instead of `succ' = succ` is a no-op, because that branch
is only reachable when `succ[c] = 0`, where the two are identical. It passed
TLC — not because the invariant is weak, but because the mutation was vacuous.
Recorded because "my mutant passed" is exactly the kind of result that gets
mistaken for "my invariant is too weak".

## Reproduction

`tla2tools.jar` is **not vendored** (the Envelope artifact has the same gap).
Fetch it, then run from this directory:

```bash
curl -sSL --retry 3 -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.7.1/tla2tools.jar
java -cp /tmp/tla2tools.jar tlc2.TLC -workers 1 LeafLock.tla
```

The teeth test is done by editing `LeafLock.tla` per the table above and re-running
the same command; no separate harness is checked in.
