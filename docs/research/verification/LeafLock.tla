------------------------------ MODULE LeafLock ------------------------------
(***************************************************************************)
(* Page-table node lock (MCS) -- TLA+ model of the handoff.                *)
(*                                                                          *)
(* Modelled from kernel/mm/pt.c: mcs_lock() / mcs_unlock() and the          *)
(* pred->next link, abstracted to per-CPU state plus the successor edges.   *)
(*                                                                          *)
(* This protocol shipped with two independent fatal bugs.  The waiter never  *)
(* published itself to its predecessor's ->next, so no unlock could ever     *)
(* grant it; and the unlocker recovered its own node from m->node, a field   *)
(* every acquirer overwrote, so it could hand the lock to nobody.  Either   *)
(* one wedges a node permanently on first contention, and neither reports   *)
(* anything: the waiter is not self-deadlocked, it waits for a handoff the   *)
(* protocol cannot produce.  No review caught it and no pre-Phase-3         *)
(* workload reached it, because mm->lock kept node-lock contention at zero.  *)
(*                                                                          *)
(* So the model is built to fail if either bug returns:                     *)
(*   WaiterLinked     -- bug 1: a waiter unreachable from any predecessor   *)
(*   FreedHasNoWaiter -- bug 2: the lock freed while someone still waits     *)
(* Both are checked in LeafLock.cfg.                                        *)
(***************************************************************************)
EXTENDS FiniteSets, Naturals

CONSTANT CPUS

ASSUME CPUS # {}

States == {"idle", "waiting", "holding"}

VARIABLES lock, state, succ

vars == <<lock, state, succ>>

Waiting == { c \in CPUS : state[c] = "waiting" }
Holding == { c \in CPUS : state[c] = "holding" }

TypeOK ==
  /\ lock \in (CPUS \cup {0})
  /\ state \in [CPUS -> States]
  /\ succ \in [CPUS -> (CPUS \cup {0})]

Init ==
  /\ lock = 0
  /\ state = [c \in CPUS |-> "idle"]
  /\ succ  = [c \in CPUS |-> 0]

(***************************************************************************)
(* Enter(c) -- the acquire half.  On contention the ONLY thing that makes   *)
(* the waiter reachable is succ'[lock] = c.  Drop that one conjunct and    *)
(* the waiter is orphaned: WaiterLinked fails.                              *)
(***************************************************************************)
Enter(c) ==
  /\ c \notin Holding
  /\ state[c] = "idle"
  /\ IF lock = 0
        THEN /\ lock' = c
             /\ state' = [state EXCEPT ![c] = "holding"]
             /\ succ' = [succ EXCEPT ![c] = 0]
        ELSE /\ lock' = c
             /\ state' = [state EXCEPT ![c] = "waiting"]
             /\ succ' = [succ EXCEPT ![c] = 0, ![lock] = c]

(***************************************************************************)
(* Grant(p, s) -- the unlocker hands its own node to its own successor.     *)
(* Reading a different node than the unlocker's own is bug 2: it grants the  *)
(* wrong CPU, or (in Exit below) frees the lock with s still waiting.        *)
(***************************************************************************)
Grant(p, s) ==
  /\ p \in Holding
  /\ succ[p] = s
  /\ state[s] = "waiting"
  /\ lock' = lock
  /\ state' = [state EXCEPT ![p] = "idle", ![s] = "holding"]
  /\ succ' = [succ EXCEPT ![p] = 0]

(***************************************************************************)
(* Exit(c) -- the release half.  The CAS-on-tail branch is only sound       *)
(* because no successor enqueued in the window; FreedHasNoWaiter is what    *)
(* catches a release that ignores the successor entirely.                   *)
(***************************************************************************)
Exit(c) ==
  /\ c \in Holding
  /\ IF succ[c] # 0
        THEN /\ lock' = lock
             /\ state' = [state EXCEPT ![c] = "idle", ![succ[c]] = "holding"]
             /\ succ' = [succ EXCEPT ![c] = 0]
        ELSE /\ lock' = 0
             /\ state' = [state EXCEPT ![c] = "idle"]
             /\ succ' = succ

Next == (\E c \in CPUS : Enter(c)) \/ (\E p \in CPUS, s \in CPUS : Grant(p, s))
        \/ (\E c \in CPUS : Exit(c))

Spec == Init /\ [][Next]_vars

(***************************************************************************)
(* Safety                                                                  *)
(***************************************************************************)

(* At most one CPU is in the critical section for a given node. *)
MutualExclusion == Cardinality(Holding) <= 1

(* BUG 1's victim.  Every waiter must be the successor of some CPU, or no   *)
(* release can ever find it.  With the succ'[lock] = c conjunct removed,    *)
(* this fails the moment two CPUs contend. *)
WaiterLinked == \A c \in Waiting : \E p \in CPUS : succ[p] = c

(* BUG 2's victim.  A node whose lock word is free has no one waiting on    *)
(* it.  An unlocker that clears the lock without handing off violates this. *)
FreedHasNoWaiter == (lock = 0) => Waiting = {}

(* A successor is always a real waiter, never a stale pointer. *)
SuccIsWaiter == \A p \in CPUS : succ[p] # 0 => state[succ[p]] = "waiting"

(***************************************************************************)
(* Liveness -- an enqueued waiter is eventually handed the lock.  Needs     *)
(* weak fairness on Enter and Exit, matching a scheduler that eventually    *)
(* runs every runnable CPU.                                                 *)
(***************************************************************************)
WaiterProgress == \A c \in CPUS : (state[c] = "waiting") ~> (state[c] = "holding")

Fairness == /\ \A c \in CPUS : WF_vars(Enter(c))
            /\ \A c \in CPUS : WF_vars(Exit(c))

SpecFair == Spec /\ Fairness
=============================================================================
