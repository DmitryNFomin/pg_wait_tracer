D01: D12[U] → D15[T] → F15. b_D=12 via fresh-record route. Startup gap closed: C_close=12, persisted P_close=13; pending=0; census complete. X duration/count in [12,16): Unavailable. In [13,16): duration=2 ns, count=1. Completed-Y count in [12,13): Unavailable.
why: DECISIONs 37(a), 41–42: the uncertified anchor at C_close requires the persisted endpoint increment.

D02c: D9[U] → D10[T] → F12. b_D=9 via fresh-record route. Startup gap closed: C_close=9, P_close=10; pending=0; census complete. [10,13) passes startup coverage. X remains open: completed count=0; no duration is available from the supplied evidence.
why: DECISIONs 37(a), 41–42 and the D02c clarification. The ordinary reread establishes no baseline; an open wait is not reported as zero duration.

D03: D12[U] → D15[T] → F15, all after AD_D8. b_D=12 via fresh-record route. Startup gap closed: C_close=12, P_close=13; pending=0; census complete.
why: DECISIONs 36–37 and 41–42. Missing ADMIT holds the eligible batch and its frontier; release follows semantic order.

D09: D12[U] → D15[T] → terminal_D18[T] → EXIT_D18 → FORK_R20 → R22[U] → F22. b_D=12 via discovery fresh-record route; R’s direct-fork coverage lower bound is 22. Startup gap closed: C_close=12, P_close=13; pending=0; census complete. Delayed cleanup preserves the tombstone; retry cannot reinstall D. No VOID or UNTRACED.
why: DECISIONs 25a–25c, 34–35, 37(a), 41–43. Queued cookies preserve D’s identity; D’s EXIT precedes R’s FORK, and does not replace D’s established lower bound.

D10: D12[U] → D15[T] → F15 → F30. Initially b_D=12 via fresh-record route and P_close=13. LATE_U(40) revokes that startup closure: gap open again from boot_start; pending=1 (U); census completeness unknown; b_U absent. All D01 numerical answers are now Unavailable, including cached answers. No merge VOID.
why: DECISION 43 and the D10 clarification require startup-coverage revocation without an observation-only VOID.

Q02: D15[U] → F15. Quiet confirmation rejected; producer-written CPU/query/command fields remain advanced, without rollback. b_D=15 via fresh-record route. Startup gap closed: C_close=15, P_close=16; pending=0; census complete. X duration/count and CPU in [12,16): Unavailable. No VOID.
why: DECISIONs 35, 37–38, 41–42. Changed voluntary-switch counts reject the proposed baseline, while preserving producer writes.

Q03: D15[U] → F15. b_D=15 via fresh-record route. Startup gap closed: C_close=15, P_close=16; pending=0; census complete. X duration/count and CPU in [12,16): Unavailable. No VOID.
why: DECISIONs 37–38 and 41–42. S1=running fails quiet confirmation; D15 becomes the uncertified fresh-record anchor.

Q04: D15[U] → F15. b_D=15 via fresh-record route. Startup gap closed: C_close=15, P_close=16; pending=0; census complete. X duration/count and CPU in [12,16): Unavailable. No VOID.
why: DECISIONs 37–38 and 41–42. Equal scheduler counters and state letters do not establish the required inactivity proofs.

Q04b: D15[U] → F15. b_D=15 via fresh-record route. Startup gap closed: C_close=15, P_close=16; pending=0; census complete. X duration/count and CPU in [12,16): Unavailable. No VOID.
why: DECISIONs 37–38 and 41–42. Zero schedstat values do not establish the required inactivity proofs.

Q05: D15[U] → F15. b_D=15 via fresh-record route. Startup gap closed: C_close=15, P_close=16; pending=0; census complete. X duration/count and CPU in [12,16): Unavailable. No VOID; counters remain C0.
why: DECISIONs 37–38 and 41–42. EACCES rejects this bracket; the row specifies no independently failed required read or accounting operation.

Q06: D15[U] → F15. b_D=15 via fresh-record route. Startup gap closed: C_close=15, P_close=16; pending=0; census complete. X duration/count and CPU in [12,16): Unavailable. No VOID.
why: DECISIONs 37–39 and 41–42. Threads=2 rejects quiet confirmation; the independently qualified final attachment permits the fresh-record route.

D14a: A12[T] → D12[U] → D15[T] → F15. b_D=12 via fresh-record route. Startup gap closed: C_close=12, P_close=13; pending=0; census complete. Combined completed-Y count in [12,13): Unavailable, although A individually supplies one completion. X in [13,16): duration=2 ns, count=1.
why: §2 orders A before D at timestamp 12. DECISIONs 37(a), 41–42 make the persisted startup exclusion apply to the whole requested window.

D14b: D12[U] → D15[T] → F15. b_D=12 via fresh-record route. Startup gap closed: C_close=15, P_close=15; pending=0; census complete. X in [15,16): available, duration=0 ns, completed count=1.
why: DECISIONs 41–42. The census fence sets C_close=15; D15 is certified, so no increment applies. Half-open clipping excludes X’s duration before 15 but includes its completion at 15.

D14c: D11[U] → F11 → D15[T] → F15. b_D=11 via quiet-baseline route throughout. After the first release, the committed baseline remains α3’s X at time 11, CPU accumulator/base=0, valid=true, on_cpu_ts=0; D11 does not rebase it. After the second release, wait/time advance normally to 0 at 15; b_D remains 11. Startup gap closed: C_close=11, P_close=12; pending=0; census complete. X duration/count in [11,16): Unavailable. In [12,16): duration=3 ns, count=1.
why: DECISIONs 35–38 and 41–42. Retained physical history establishes D11 as an earlier-attempt record despite timestamp equality; its uncertified completion at C_close triggers the increment.

A01: F12 → F15. No DATA published. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=2.
why: DECISIONs 36, 41 and 45. AB_D releases the missing-ADMIT dependency; D9 and D12 are validated discards, without resolving D.

A02: F12 → F15 → F19. No DATA published. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=3.
why: DECISION 45 applies permanently to future D records; D17 passes validated release consumption at S(20).

A03: F12 → F15 → F17 → EXIT_D18 → F18. b_D absent; terminal discharge x_D=18. Startup gap closed: C_close=18, P_close=19; pending=0; census complete. discovery_abandoned_records=3. No UNTRACED; D contributes neither an A2 birth nor omission.
why: DECISIONs 40–43 and 45. The terminal TRANS is discarded at timestamp C_close, requiring the increment; EXIT publishes only with W>18.

A04: D12[U] → F12 → VOID20[C0,unknown_evidence]. Suspended. b_D=12 via fresh-record route; b_H absent. Startup gap open; pending=1 (H); census complete. discovery_abandoned_records=0.
why: DECISIONs 37(a), 41 and 45. D is already admitted and cannot be abandoned; H still prevents startup closure.

A05: VOID20[C0,unknown_evidence]. Suspended; no DATA or FRONTIER. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=0.
why: DECISION 45. ADMIT after accepted abandonment is contradictory; D9 has not passed validated release consumption.

A06: VOID20[C0,unknown_evidence]. Suspended; no DATA or FRONTIER. b_D absent. Startup gap open; pending=1 (D); census complete. discovery_abandoned_records=0.
why: DECISION 45 permits abandonment only for an inventoried, bound discovery identity; H is absent from the census.

A07: H15[T] → F15. D9 is discarded. b_H=12 via quiet-baseline route; b_D absent. Startup gap open; pending=1 (D); census complete. discovery_abandoned_records=1.
why: DECISIONs 37(b), 41 and 45. H’s confirmed seed certifies H15; D’s abandonment neither establishes a lower bound nor discharges its inventory entry.

A08: VOID20[Ce,counter_change]. Suspended; no DATA or FRONTIER. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=0.
why: §4 and DECISION 45. The counter change occurs before any validated discard; abandonment forgives no loss.

A09: F12 → F15 → F17 → EXIT_D18 → F18. b_D absent; x_D=18. Startup gap closed: C_close=18, P_close=19; pending=0; census complete. discovery_abandoned_records=3. The duplicate AB_D adds no output or diagnostic increment. No UNTRACED.
why: DECISIONs 40–42 and 45. Identical retransmission remains idempotent after discharge and gap closure.

A10: F12 → F15 → VOID20[C0,unknown_evidence]. Suspended. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=2.
why: DECISION 45 makes every later CONFIRM for the abandoned identity contradictory; previously completed discards remain counted.

A11: F12 → F15 → VOID20[C0,seal_violation]. Reject D14, suspend, and mark/report the epoch invalid because its F15 is disproved. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=2.
why: §3–4 and DECISION 45. D14 is below P0’s retained boundary 16, but above its preceding DATA timestamp 12; arrival validation precedes discard.

A12: F12 → F15 → VOID20[C0,allocation_fail] → F17. REARM at 21 is accepted; the final epoch is active. D17 is discarded after fresh source completeness. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=3.
why: DECISIONs 27–27a, 41 and 45. Accepted abandonment survives REARM; D17 satisfies retained arrival boundaries, and F17 advances the retained F15.

A13: hold. No DATA or FRONTIER. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=0.
why: §3 and DECISION 45. E’s cutoff remains zero, so no record passes validated release consumption.

A14: VOID20[unknown,counter_unknown]. Suspended; no DATA or FRONTIER. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=0.
why: §4 and DECISION 45. Incomplete failure accounting causes VOID and cannot be bypassed by abandonment.

A15: F12 → F15 → F17 → VOID20[Cw,writer_push_fail]. EXIT_D18 is rejected; no F18. Suspended. b_D absent; terminal discharge x_D=18 is accepted. Startup gap remains open with no persisted close; pending=0; census complete. discovery_abandoned_records=3. No UNTRACED.
why: DECISIONs 40–42 and 45 distinguish accepted terminal discharge from successful closure publication. The completed terminal discard remains counted, but rejected supporting EXIT prevents gap closure.

A16: VOID20[C0,unknown_evidence]. Suspended; no DATA or FRONTIER. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=0.
why: DECISION 45 forbids uncertain record attribution from becoming a successful discard; applicable identity validation still faults.

A17: F12 → F15 → F17 → EXIT_D18 → F18 → FORK_R20 → R22[U] → F22. b_D absent; x_D=18. R’s direct-fork coverage lower bound is 22. Startup gap remains closed: C_close=18, P_close=19; pending=0; census complete. discovery_abandoned_records=3. No UNTRACED.
why: DECISIONs 25a–25c, 34–35 and 40–45. Abandonment belongs to D’s lifetime identity; it does not discard or certify R’s records.

A18: VOID20[C0,allocation_fail]. Suspended; no DATA or FRONTIER. AB_D is accepted without rearming. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=0.
why: §4 and DECISION 45. The fault discards the tentative suffix; abandonment during suspension supplies no trusted release or completed discard.

A19: F15. No TEXT or DATA published. b_D absent. Startup gap open; pending=1; census complete. discovery_abandoned_records=3: TEXT_Dx, D9 and M_D10.
why: DECISION 45 discards exclusive numerical metadata and TRANS/MARKER records after complete validation, including dependencies; untimed TEXT establishes no timestamp or lower bound.

A20: EXIT_D18 → F18 → VOID20[C0,unknown_evidence]. Suspended. b_D absent; x_D=18. Startup gap remains closed: C_close=18, P_close=18; pending=0; census complete. discovery_abandoned_records=0. No invented terminal TRANS or UNTRACED.
why: DECISIONs 40–43 and 45. Lifecycle EXIT alone requires no endpoint increment; a first abandonment after startup-gap closure is invalid.