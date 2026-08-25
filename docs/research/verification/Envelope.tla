----------------------------- MODULE Envelope -----------------------------
(***************************************************************************)
(* Capability envelope mediator core -- TLA+ model (docs/research/05).      *)
(* Narrowed C4 artifact per docs/research/08: the mediator state machine,   *)
(* abstracted from kernel/ipc/envelope.c.                                   *)
(*                                                                          *)
(* One envelope.  A supervisor task creates it and never enters; worker     *)
(* tasks enter monotonically (fork = shared-root attachment, execve cannot  *)
(* shed it -- there is no Leave action).  Mediated operations:               *)
(*   Acquire     -- create-time acquisition (A1-A3), rights <= class cap    *)
(*   TransferIn  -- A6/A7 descriptor transfer, grant clamped to cap,        *)
(*                 empty grant denies                                       *)
(*   Reopen      -- A8 /proc/self/fd reopen: rights = request INTERSECT     *)
(*                 source shadow (monotone non-upgrade)                     *)
(*   SendOut     -- A6 send side, gated by propagation_types                *)
(*   Use         -- tracked authorities enforce the direction bit           *)
(*   UseX        -- grandfathered authority: exempt from direction, charged *)
(*   LazyExpire / Revoke / Kill                                             *)
(*                                                                          *)
(* Safety + fairness-liveness: under weak fairness on Tick/LazyExpire/Kill, *)
(* the time budget surely lapses (<>expired), expiry surely escalates to    *)
(* kill (expired ~> killSent), and post-expiry every mediated operation is  *)
(* disabled -- denial-by-construction (DenyAfterExpiry).                    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

(***************************************************************************)
(* Concrete model constants (narrowed C4 instantiation; the kernel keeps   *)
(* these in the runtime policy struct).                                    *)
(***************************************************************************)
OpsMax == 3                 \* initial op budget
DataMax == 4                \* initial data budget
ExpireAt == 3               \* tick at which the time budget lapses; 0 = none
MaxTick == 6                \* clock ceiling
Supervisor == "sup"
Tasks == {"sup", "w1", "w2"}
Gfds == {1, 2}
ObjTypes == {"file", "sock"}
AllowedTypes == {"file"}
Cap == [ty \in ObjTypes |-> IF ty = "file" THEN {"R", "W"} ELSE {}]
Propagation == {}
WantTransfer == {"R", "W"}
KillOnExpire == TRUE

VARIABLES ops, data, now, expired, killSent, attached, shadows

vars == <<ops, data, now, expired, killSent, attached, shadows>>

Right == {"R", "W"}

ReplaceShadow(S, g, ns) == {x \in S : x.gfd # g} \cup {ns}

HasShadow(g) == \E s \in shadows : s.gfd = g

Init ==
  /\ ops = OpsMax
  /\ data = DataMax
  /\ now = 0
  /\ expired = FALSE
  /\ killSent = FALSE
  /\ attached = {}
  /\ shadows = {}

(* enter is monotone: once inside, always inside (no Leave action) *)
Enter(t) ==
  /\ t \in Tasks \ {Supervisor}
  /\ t \notin attached
  /\ attached' = attached \cup {t}
  /\ UNCHANGED <<ops, data, now, expired, killSent, shadows>>

(* A1/A2/A3: creation-time acquisition; request beyond the cap denies, so
   only subsets of the cap succeed and each costs one op. *)
Acquire(t, gfd, ty, rgs) ==
  /\ t \in attached
  /\ ~expired
  /\ ty \in AllowedTypes
  /\ rgs \subseteq Cap[ty]
  /\ rgs # {}
  /\ ops > 0
  /\ ops' = ops - 1
  /\ shadows' = ReplaceShadow(shadows, gfd,
        [gfd |-> gfd, type |-> ty, rights |-> rgs])
  /\ UNCHANGED <<data, now, expired, killSent, attached>>

(* A7/A6 receive side: incoming authority clamped to the class cap;
   an empty grant denies (action simply not enabled in that case). *)
TransferIn(t, gfd, ty) ==
  /\ t \in attached
  /\ ~expired
  /\ ty \in AllowedTypes
  /\ WantTransfer \cap Cap[ty] # {}
  /\ ops > 0
  /\ ops' = ops - 1
  /\ shadows' = ReplaceShadow(shadows, gfd,
        [gfd |-> gfd, type |-> ty, rights |-> WantTransfer \cap Cap[ty]])
  /\ UNCHANGED <<data, now, expired, killSent, attached>>

(* A8: reopening through a proc-fd link yields request INTERSECT source
   shadow -- authority cannot be upgraded past the source instance. *)
Reopen(t, gNew, gSrc, req) ==
  /\ t \in attached
  /\ ~expired
  /\ HasShadow(gSrc)
  /\ LET src == CHOOSE s \in shadows : s.gfd = gSrc
         clamped == req \cap src.rights
     IN clamped # {}
        /\ ops > 0
        /\ ops' = ops - 1
        /\ shadows' = ReplaceShadow(shadows, gNew,
              [gfd |-> gNew, type |-> "file", rights |-> clamped])
  /\ UNCHANGED <<data, now, expired, killSent, attached>>

(* A6 send side: propagation_types gates authorities leaving the task. *)
SendOut(t, gfd) ==
  /\ t \in attached
  /\ ~expired
  /\ \E s \in shadows : s.gfd = gfd /\ s.type \in Propagation
  /\ ops > 0
  /\ ops' = ops - 1
  /\ UNCHANGED <<data, now, expired, killSent, attached, shadows>>

(* Tracked use: the direction bit must be present on the shadow. *)
Use(t, gfd, bytes, dir) ==
  /\ t \in attached
  /\ ~expired
  /\ HasShadow(gfd)
  /\ \E s \in shadows :
        s.gfd = gfd /\ (dir = "R" <=> "R" \in s.rights)
                     /\ (dir = "W" <=> "W" \in s.rights)
  /\ ops > 0
  /\ data >= bytes
  /\ ops' = ops - 1
  /\ data' = data - bytes
  /\ UNCHANGED <<now, expired, killSent, attached, shadows>>

(* Grandfathered authority (inherited before enter()): no direction
   requirement, but the root budget is still charged. *)
UseX(t, gfd, bytes, dir) ==
  /\ t \in attached
  /\ ~expired
  /\ ~HasShadow(gfd)
  /\ ops > 0
  /\ data >= bytes
  /\ ops' = ops - 1
  /\ data' = data - bytes
  /\ UNCHANGED <<now, expired, killSent, attached, shadows>>

Tick ==
  /\ now < MaxTick
  /\ now' = now + 1
  /\ UNCHANGED <<ops, data, expired, killSent, attached, shadows>>

LazyExpire ==
  /\ ~expired
  /\ ExpireAt # 0
  /\ now >= ExpireAt
  /\ expired' = TRUE
  /\ UNCHANGED <<ops, data, now, attached, shadows, killSent>>

Revoke ==
  /\ ~expired
  /\ expired' = TRUE
  /\ UNCHANGED <<ops, data, now, attached, shadows, killSent>>

Kill ==
  /\ expired
  /\ KillOnExpire
  /\ ~killSent
  /\ killSent' = TRUE
  /\ UNCHANGED <<ops, data, now, expired, attached, shadows>>

AnyOp(t) ==
  \/ \E gfd \in Gfds, ty \in ObjTypes, rgs \in SUBSET Right :
        Acquire(t, gfd, ty, rgs)
  \/ \E gfd \in Gfds, ty \in ObjTypes : TransferIn(t, gfd, ty)
  \/ \E gn \in Gfds, gs \in Gfds, rq \in SUBSET Right : Reopen(t, gn, gs, rq)
  \/ \E gfd \in Gfds : SendOut(t, gfd)
  \/ \E gfd \in Gfds, b \in {0, 1}, d \in {"R", "W"} :
        Use(t, gfd, b, d) \/ UseX(t, gfd, b, d)

Next ==
  \/ \E t \in Tasks : Enter(t) \/ AnyOp(t)
  \/ Tick
  \/ LazyExpire
  \/ Revoke
  \/ Kill

(* Weak fairness on environment/manager steps mirrors the kernel sweeper:   *)
(* ticks advance, an overdue envelope is lazily lapsed, kill follows expiry. *)
Fairness ==
  /\ WF_vars(Tick)
  /\ WF_vars(LazyExpire)
  /\ WF_vars(Kill)

Spec == Init /\ [][Next]_vars /\ Fairness

(* ---- safety invariants ---- *)

TypeAllowed   == \A s \in shadows : s.type \in AllowedTypes
RightsSubCap  == \A s \in shadows : s.rights \subseteq Cap[s.type]
OpsNonNeg     == ops >= 0
DataNonNeg    == data >= 0
ClockBounded  == now <= MaxTick

(* temporal: expiry and kill are one-way flags *)
NoResurrect == [][expired => expired']_vars
KillOnce    == [][killSent => killSent']_vars

(* fairness-liveness: mediator lifecycle closure (docs/research/08 §3) *)
ExpiryLiveness  == <>expired
KillLiveness    == expired ~> killSent
DenyAfterExpiry == expired => (\A t \in Tasks : ~ENABLED AnyOp(t))

=============================================================================
