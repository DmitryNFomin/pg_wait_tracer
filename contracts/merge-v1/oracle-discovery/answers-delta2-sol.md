T1: F18 → A20[T] → F20; startup gap open, pending=1, census complete; b_D unset (no route); uncovered records=1; discovery_abandoned_records=0.
why: DECISION 40 consumes the unresolved cookie-less EXIT without publication or VOID and releases the held frontier. DECISION 41 leaves D unresolved.

T2: VOID50[C0,unknown_evidence]; no DATA or FRONTIER; startup gap open, pending=1, census complete; b_D unset (no route); discovery_abandoned_records=0.
why: DECISION 40 holds the EXIT and frontier until a final disposition; finishing capture without one faults. The held batch includes A20.

T3: F18 → A20[T] → F20 → VOID20[C0,unknown_evidence]; startup gap open, pending=1, census complete; b_D unset (no route); uncovered records=1; discovery_abandoned_records=0.
why: DECISION 40 makes a later matching disposition contradictory after an unresolved disposition; it supplies no discharge.

T4: F18; no DATA; startup gap open, pending=1, census complete; b_D unset (no route); uncovered records=1; discovery_abandoned_records=0.
why: DECISION 40’s unresolved disposition permits consumption of the matching cookie-less EXIT without publication or VOID; it supplies neither discharge nor boundary.

V1: EXIT_D18 → VOID20[Ce,counter_change] → FRONTIER(19,Ce); startup gap open, pending=1, census complete; b_D unset (no route); discovery_abandoned_records=0.
why: DECISIONs 40–42 and the terminal-discharge clarification: the post-EXIT counter fault prevents discharge permanently for that EXIT. REARM and later TERM cannot commit it.

V2: EXIT_D18 → VOID20[Ce,counter_change] → FRONTIER(19,Ce); startup gap open, pending=1, census complete; b_D unset (no route); discovery_abandoned_records=0.
why: The terminal-discharge clarification applies regardless of TERM/EXIT order. The failed release attempt commits neither discharge nor startup-gap closure.

V3: EXIT_D17 → VOID20[Cw,writer_push_fail] → FRONTIER(19,Cw); A18 is not published; startup gap open, pending=1, census complete; b_D unset (no route); discovery_abandoned_records=0.
why: §3 excludes rejected publications. The terminal-discharge clarification prevents EXIT_D17 from discharging D because the same release attempt subsequently fails.

V4: EXIT_D18 → VOID20[Cw,writer_push_fail] → FRONTIER(19,Cw); F18 is not published; startup gap open, pending=1, census complete; b_D unset (no route); discovery_abandoned_records=0.
why: The terminal-discharge clarification requires successful FRONTIER publication when due. Rejecting F18 prevents discharge and closure; later TERM cannot revive that EXIT.