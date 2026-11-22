# Independent answer set (opus): merge-v1 delta 9

Written from the amended contract, ROWS.md and TASK-delta9.md alone.

## First packet (11 executions)

# Answers (opus): merge-v1 delta 9

Derived only from CONTRACT.md (§§2-6, DECISIONs 30-45), CONTRACT-coverage-a2-v1.md, ROWS.md and TASK.md in this directory. No product, test or earlier answer was read.

## 0. Common conventions

- Defaults (H rows = G-row defaults): mode N; sources {P0,P1,E,L}; D=(pid 30, g=2^63) installed and independently bound; census {D} complete at fence 5 (H13: {D,H}); C0 everywhere; detection 20 (22 after RA, finish as stated). A and B attached. Epoch active, all cutoffs 0, no frontier, startup gap open.
- `OPEN` = initial plain startup GAP_OPEN at boot_start (already persisted before the row's first input). `O!` = revoked startup OPEN (none occurs in any execution below). `END(p)` = startup GAP_CLOSE with endpoint p, reserved field 0.
- Source positions start at 1 per source and advance in consumption order, boundary records included (§5). `S(w)` = `BOUNDARY(P0,w)`, `BOUNDARY(P1,w)`, `BOUNDARY(E,w)`, `BOUNDARY(L,w)`, each with its next position, each evaluated separately (§5).
- TERM_Dt = matched final terminal disposition for D at t naming one EXIT transport identity. Controls (AD, TERM, RA, finish) consume no source position.
- Accessor = exit-disposition accessor of the named EXIT: NONE / MATCHED(t, lifetime[, refuted]) / UNRESOLVED.
- "Rej-hist" = rejected-control refutation history (D40), distinct from the retained final terminal claim ("claim").
- Pending = L+Q (lifetimes + numeric observations). Q=0 throughout.
- Faults: every FAULT here is `unknown_evidence`; each separately delivered fault report produces one VOID (§4 "Emit VOID for each separately delivered fault report, including during suspension"). Each increments only the unknown_evidence fault diagnostic; no storage counter changes anywhere (all checks observe C0); no epoch invalidation (no published frontier is disproved); no nonzero exit (no internal breach); abandonment consumption 0, uncovered consumption 0, `discovery_abandoned_records` 0 in every execution unless stated.
- Startup availability of [50,60) at any VOID entry: the persisted reader sees only the plain OPEN with no END, so [boot_start,∞) is inside the open startup gap; [50,60) is Unavailable on the startup restriction alone, regardless of the otherwise valid supplied coverage, attribution and A2 evidence (D41: "The interval [boot_start,P_close) is unavailable"; no P_close exists). No revocation or cache invalidation is required at any VOID below, because nothing was ever committed (D43: "Refuting a candidate that never committed does not itself publish a revoked OPEN").

Key rules cited repeatedly:
- R-REJ (D40): "A terminal control contradicted on delivery by evidence already retained for its lifetime—the timestamp or transport identity of the EXIT it names or that is independently bound, including a retained undecided cookie-less EXIT, or an attributable TRANS/MARKER stamped after its claimed time—is rejected whether or not an ADMIT or a Decision 36 contradiction exists. It reports one FAULT(unknown_evidence,d,C), records no final terminal claim, binding or disposition, leaves the exit-disposition accessor unchanged, and keeps only the refutation history needed to silence its identical retransmission."
- R-LATER (D40): "The rejected control is not a retained final terminal claim: a later otherwise qualified control is recorded without a fault for disagreeing with it."
- R-CONFLICT (D40): "A separately delivered terminal control that conflicts ... with a retained final terminal claim reports FAULT(unknown_evidence,d,C), including when the retained claim is refuted and no discharge ever committed."
- R-RETRANS (D40): "recognize an identical retransmission against that history before applying the conflicting-final-claim rule, so it produces no repeated contradiction report even after the different claim was recorded."
- R-D36TERM (D36): "An otherwise qualified matched terminal control with terminal time x<e establishes this same contradiction against a recorded ADMIT on delivery even before its named EXIT DATA arrives: record the terminal claim, refute both recorded claims, report one FAULT"; D40: such a control "is recorded as the final MATCHED disposition ... with the terminal claim marked refuted and supplying no discharge ... The exit-disposition accessor returns MATCHED after that recording, including for a cookie-less EXIT; it satisfies the requirement for one final disposition".
- R-DISPOSE (D40): "An EXIT payload disposed of by fault handling or during suspension contributes no pending payload record and is not an unfinished record merely because it was previously staged; with its final disposition recorded, it causes no unfinished-record fault at capture finish, including after REARM. Preserve an unsatisfied cookie-less EXIT final-disposition obligation separately ... If capture finishes with that obligation still unsatisfied, report FAULT(unknown_evidence,d,C) at finish even when the epoch is already suspended".
- R-LATCH (D43): first fault requiring VOID latches certification off before fault publication; afterwards no new bound, discharge or END commits; "A valid subsequent control is recorded without a fault caused by the latch".

---

## H11g — `AD_D19, EXIT_D18@L{no cookie}, TERM_D18, RA, S(26), finish(40,C0)`

### 1. Primitive inputs
| # | Input | Role |
|---|---|---|
| 1 | ADMIT_DISCOVERY(D,α,e=19,proof,20,C0) | accepted: admission recorded (admitted=true) |
| 2 | DATA(L,o=1,t=18,pid 30,no cookie,EXIT) | staged; undecided cookie-less EXIT; held until final disposition (D40) and by W=0 |
| 3 | TERM_D18 (matched, D, names L/1, x=18) | establishes D36 contradiction x=18<e=19: records final MATCHED disposition (refuted), refutes ADMIT proof, FAULT #1 (sets latch) |
| 4 | REARM(21,C0, fresh A/B + D continuing attachment) | accepted (epoch suspended); new epoch active |
| 5a-5d | BOUNDARY(P0,26,1), BOUNDARY(P1,26,1), BOUNDARY(E,26,1), BOUNDARY(L,26,2) | 5d publishes F25 |
| 6 | finish(40,C0) | no fault (disposition recorded; payload disposed) |

Commitments: none (latch from input 3; no bound, no discharge, no END).

### 2. Successful merge stream
`VOID20[C0,unknown_evidence] → FRONTIER(25,C0)` (F25 at detection 22). No TEXT/DATA output; no prior_interval_certified markings. Rejected publication calls: none.

### 3. Publication timeline
`OPEN(boot_start)` → `VOID20[C0,unknown_evidence]` (input 3; latch set before it) → [REARM baseline C0] → `F25` (input 5d; counter checkpoint C0 before it) → finish (checkpoint C0, no publication). No END, no O!, no extra OPEN.

### 4. State after each input
| After | Epoch | Latch | admitted / ADMIT proof | Claim (EXIT, t, state) | Rej-hist | Accessor L/1 | b_D/x_D | Pending L+Q=total | Census | Gap / payload |
|---|---|---|---|---|---|---|---|---|---|---|
| start | active | clear | false | none | none | (not yet named) NONE | –/– | 1+0=1 | known | open; 0 payloads |
| 1 AD_D19 | active | clear | true / unrefuted | none | none | NONE | –/– | 1 | known | open; 0 |
| 2 EXIT | active | clear | true / unrefuted | none | none | NONE (undecided, PID 30, t18) | –/– | 1 | known | open; 1 pending payload (EXIT) |
| 3 TERM_D18 | suspended | set | true / refuted | MATCHED L/1, 18, D, refuted | none | MATCHED(18,D), refuted claim | –/– | 1 | known | open; 0 (EXIT disposed) |
| 4 RA | active (e1) | set | true / refuted (RA restores nothing) | unchanged | none | MATCHED(18,D) refuted | –/– | 1 | known | open; 0 |
| 5a-5c | active | set | same | same | none | MATCHED refuted | –/– | 1 | known | open; 0; W=0 |
| 5d | active | set | same | same | none | MATCHED refuted | –/– | 1 | known | open; 0; F25 |
| 6 finish | finished | set | same | same | none | MATCHED refuted | –/– | 1 | known | open; 0; unfinished_records=0 |

No C_close/P_close is defined (D41: computed only after discharge of every inventory entry).

Input 3 reasoning: the EXIT had no independent binding, so input 2 established no D36 contradiction (D36 applies to "that lifetime's independently bound EXIT"). TERM_D18 is not separately false (names L/1, timestamp 18 matches), so R-REJ does not apply; its only contradiction is enable/termination, so R-D36TERM applies: record MATCHED (refuted), refute the recorded ADMIT proof, one FAULT. Fault handling discards the staged EXIT payload (§4).

### 5. VOID20 entry
Epoch suspended; latch set; admitted=true, ADMIT proof refuted; claim MATCHED L/1 t18 D refuted; accessor MATCHED; no rej-hist; no b/x; pending 1+0=1; census known; gap open. Persisted prefix: `OPEN` only. No revocation, no O!, no cache effect. [50,60): Unavailable (open startup gap).

### 6. Counts
Faults: 1 (unknown_evidence). Storage counters: no change. Payload disposal: the EXIT payload at input 3 (not consumption). Abandonment 0; uncovered 0; invalidation none; nonzero exit no.

### 7. Payload / obligation accounting
- Pending payload records: 1 after input 2; 0 after TERM and after every later input (RA, each S(26) constituent, finish).
- unfinished_records at finish: 0 (R-DISPOSE: disposed payload with its final disposition recorded).
- Unresolved inventory: D remains unresolved (L=1) throughout and at finish; this is not a finish fault — it only keeps the startup gap open.
- Final-disposition obligation for the cookie-less EXIT: created at input 2, satisfied at input 3 (the refuted MATCHED counts, R-D36TERM). At finish: accessor MATCHED, fault total 1 (same as after TERM). No obligation justifies a finish fault.

---

## H12a — `EXIT_D18@L (cookie, bound to D), TERM_D19{names it}, TERM_D18`

### 1. Inputs
| # | Input | Role |
|---|---|---|
| 1 | DATA(L,1,18,D,EXIT) | staged (W=0); eligible for publication without ADMIT (D40 cookie-bearing rule) |
| 2 | TERM_D19 names L/1, claims 19 | rejected on delivery (EXIT stamped 18): FAULT #1, latch set |
| 3 | TERM_D18 names L/1 at 18 | accepted disposition: final MATCHED recorded, unrefuted, no fault |

No commitment (no discharge: EXIT payload disposed, epoch suspended, latch set).

### 2. Stream
`VOID20[C0,unknown_evidence]`. No DATA/FRONTIER. Rejected publication calls: none.

### 3. Timeline
`OPEN` → `VOID20[C0,unknown_evidence]` (input 2). No END/O!.

### 4. State
| After | Epoch | Latch | admitted | Claim | Rej-hist | Accessor L/1 | b/x | Pending | Census | Gap/payload |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | active | clear | false | none | none | NONE | –/– | 1 | known | open; 1 payload |
| 2 | suspended | set | false | none | {TERM_D19 matched D L/1 t19} | NONE (unchanged, R-REJ) | –/– | 1 | known | open; 0 |
| 3 | suspended | set | false | MATCHED L/1 18 D, unrefuted, non-discharging | unchanged | MATCHED(18,D) | –/– (latch; D40 "supplies x_i only when ... the complete release attempt commits") | 1 | known | open; 0 |

### 5. VOID20 entry
Suspended; latch set; admitted false; no claim; rej-hist {TERM_D19}; accessor NONE; pending 1; census known; gap open; persisted prefix `OPEN`. No revocation/cache effect. [50,60) Unavailable.

### 6. Counts
Faults 1. No storage change. EXIT payload disposed at input 2. All consumption counts 0.

### 8. Comparison with H11e
H11e has a recorded ADMIT (e=19), so its EXIT arrival establishes the D36 contradiction (fault 1), TERM_D17 is rejected by R-REJ (fault 2), and TERM_D18 is recorded MATCHED but refuted (D40: "whether that control establishes the contradiction or follows its earlier report"), no fault. H12a has no ADMIT, so no enable/termination contradiction exists: only the false TERM_D19 faults, and TERM_D18 is recorded unrefuted (R-LATER). Result: H12a 1 fault, unrefuted MATCHED; H11e 2 faults, refuted MATCHED. In both, no discharge commits.

---

## H12b — `EXIT_D18@L{no cookie}, TERM_D19{names it}, TERM_D18, finish(40,C0)`

### 1. Inputs
| # | Input | Role |
|---|---|---|
| 1 | DATA(L,1,18,pid 30,no cookie,EXIT) | undecided EXIT for PID 30; held for disposition; final-disposition obligation created |
| 2 | TERM_D19 names L/1, claims 19 | rejected on delivery against the retained undecided EXIT (R-REJ "including a retained undecided cookie-less EXIT"): FAULT #1, latch |
| 3 | TERM_D18 | recorded final MATCHED (binding L/1 → D), unrefuted, no fault; obligation satisfied |
| 4 | finish(40,C0) | no fault |

### 2. Stream
`VOID20[C0,unknown_evidence]`. Rejected publications: none.

### 3. Timeline
`OPEN` → `VOID20` (input 2) → finish (checkpoint C0). No END/O!.

### 4. State
| After | Epoch | Latch | admitted | Claim | Rej-hist | Accessor L/1 | Binding of L/1 | Pending | Census | Gap/payload |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | active | clear | false | none | none | NONE | none (undecided PID 30) | 1 | known | open; 1 payload |
| 2 | suspended | set | false | none | {TERM_D19} | NONE | none (rejection accepts no binding) | 1 | known | open; 0 (disposed) |
| 3 | suspended | set | false | MATCHED L/1 18 D unrefuted | unchanged | MATCHED(18,D) | D (from TERM_D18) | 1 | known | open; 0 |
| 4 finish | finished | set | false | same | same | MATCHED(18,D) | D | 1 | known | open; unfinished 0 |

### 5. VOID20 entry
As H12a's entry (suspended, latch, no claim, rej-hist {TERM_D19}, accessor NONE, pending 1, gap open, prefix `OPEN`, [50,60) Unavailable).

### 6. Counts
Faults 1; storage none; EXIT payload disposed (input 2); consumption 0.

### 7. Accounting
- Pending payload records: 1 after input 1, then 0.
- unfinished_records at finish: 0 (R-DISPOSE, disposition recorded).
- Unresolved inventory: D, L=1, at finish (no fault from it).
- Obligation: unsatisfied after input 2 (the rejected control recorded no disposition), satisfied at input 3. At finish: accessor MATCHED, fault total 1; no finish fault.

### 8. Comparison with H11f
H11f: ADMIT e=19 recorded, so TERM_D18 itself establishes the D36 contradiction: MATCHED recorded but refuted, ADMIT proof refuted, 1 fault. H12b: no ADMIT, so no enable/termination contradiction is imported; the only fault is the false TERM_D19, and TERM_D18 is an unrefuted MATCHED. Both have accessor MATCHED at finish, no finish fault, fault total 1, and no discharge.

---

## H12c — `EXIT_D18@L{no cookie}, TERM_D19{names it}, finish(40,C0)`

### 1. Inputs
1 EXIT (staged, obligation created); 2 TERM_D19 rejected (FAULT #1, latch); 3 finish(40,C0): obligation unsatisfied → FAULT(unknown_evidence,40,C0) (#2), missing-disposition fault, records no disposition (R-DISPOSE).

### 2. Stream
`VOID20[C0,unknown_evidence] → VOID40[C0,unknown_evidence]`. Rejected publications: none.

### 3. Timeline
`OPEN` → `VOID20` (input 2) → `VOID40` (finish). No END/O!.

### 4. State
| After | Epoch | Latch | Claim | Rej-hist | Accessor | Pending | Census | Gap/payload |
|---|---|---|---|---|---|---|---|---|
| 1 | active | clear | none | none | NONE | 1 | known | open; 1 payload |
| 2 | suspended | set | none | {TERM_D19} | NONE | 1 | known | open; 0 |
| 3 finish | suspended/finished | set | none | {TERM_D19} | NONE | 1 | known | open; unfinished 0 |

admitted=false throughout; no b/x.

### 5. VOID entries
VOID20: as H12b. VOID40: suspended, latch set, no claim, rej-hist {TERM_D19}, accessor NONE, obligation unsatisfied, pending 1, census known, gap open; persisted prefix `OPEN, VOID20`. No revocation. [50,60) Unavailable.

### 6. Counts
Faults 2 (unknown_evidence ×2). Storage none. Payload disposed at input 2. Consumption 0.

### 7. Accounting
Pending payload records 0 after input 2 and at finish; unfinished_records 0 (disposed payload is "not an unfinished record merely because it was previously staged"). Unresolved inventory D (L=1). The unsatisfied cookie-less EXIT final-disposition obligation is the retained obligation that justifies the finish fault; it is "not another report of the rejected control's contradiction". After TERM: accessor NONE, fault total 1; at finish: accessor NONE, fault total 2.

---

## H12d — `D20@P0, TERM_D19, RA, TERM_D21` (both name prospective EXIT L/1; no EXIT DATA)

### 1. Inputs
| # | Input | Role |
|---|---|---|
| 1 | DATA(P0,1,20,D,TRANS) | staged; held by missing ADMIT (D36) and W=0; w[P0]=20; attributable TRANS at 20 retained in D's summary |
| 2 | TERM_D19 names L/1 | rejected on delivery: retained attributable TRANS at 20 > 19 (R-REJ); FAULT #1, latch |
| 3 | REARM(21,C0,fresh A/B; no D proof — no recorded ADMIT) | accepted; epoch e1 active |
| 4 | TERM_D21 names L/1 (detection 22) | recorded final MATCHED claim, unrefuted, no fault (R-LATER; D20 at 20 is not after 21) |

### 2. Stream
`VOID20[C0,unknown_evidence]`. Rejected publications: none.

### 3. Timeline
`OPEN` → `VOID20` (input 2) → [RA baseline C0]. No END/O!.

### 4. State
| After | Epoch | Latch | Claim | Rej-hist | Accessor L/1 (prospective) | Pending | Census | Gap/payload |
|---|---|---|---|---|---|---|---|---|
| 1 | active | clear | none | none | NONE | 1 | known | open; 1 payload (D20) |
| 2 | suspended | set | none | {TERM_D19 L/1 t19} | NONE | 1 | known | open; 0 (D20 disposed; summary retained) |
| 3 | active | set | none | retained across REARM | NONE | 1 | known | open; 0 |
| 4 | active | set | MATCHED L/1 21 D unrefuted (prospective; would apply when EXIT consumed) | retained | MATCHED(21,D) | 1 | known | open; 0 |

admitted=false; no b/x (latch; no EXIT).

### 5. VOID20 entry
Suspended, latch, no claim, rej-hist {TERM_D19}, accessor NONE, pending 1, census known, gap open, prefix `OPEN`. No revocation. [50,60) Unavailable.

### 6. Counts
Faults 1. Storage none. D20 payload disposed (not abandonment consumption — no ABANDON). Consumption 0.

### 8. Comparison with H6c
In H6c TERM_D19 arrives first with nothing contradicting it and is validly recorded (accessor MATCHED 19); D20 later refutes it (fault 1), so it remains "the retained refuted claim, against which a separately delivered conflicting control still faults": TERM_D21 conflicts in terminal time → fault 2 (R-CONFLICT). In H12d D20 is retained first, so TERM_D19 is rejected on delivery, never becomes a retained claim, and TERM_D21 is recorded without a fault (R-LATER). Order of arrival therefore decides 1 vs 2 faults and which claim (19 refuted vs 21 unrefuted) is the final MATCHED.

---

## H12e — `EXIT_D18@L, TERM_D19, TERM_D19(retrans), TERM_D18, TERM_D19(retrans)` (H12a facts)

### 1. Inputs
1 EXIT staged; 2 TERM_D19 rejected (FAULT #1, latch); 3 identical retransmission — matched against rej-hist, no report; 4 TERM_D18 recorded MATCHED unrefuted, no fault; 5 identical retransmission — R-RETRANS applies before R-CONFLICT, no report.

### 2. Stream
`VOID20[C0,unknown_evidence]`. Rejected publications: none.

### 3. Timeline
`OPEN` → `VOID20` (input 2).

### 4. State
| After | Epoch | Latch | Claim | Rej-hist | Accessor | Faults | Pending | Gap/payload |
|---|---|---|---|---|---|---|---|---|
| 1 | active | clear | none | none | NONE | 0 | 1 | open; 1 |
| 2 | suspended | set | none | {TERM_D19} | NONE | 1 | 1 | open; 0 |
| 3 | suspended | set | none | {TERM_D19} | NONE | 1 | 1 | open; 0 |
| 4 | suspended | set | MATCHED 18 D unrefuted | {TERM_D19} (survives recording of a different claim) | MATCHED(18,D) | 1 | 1 | open; 0 |
| 5 | suspended | set | unchanged | unchanged | MATCHED(18,D) | 1 | 1 | open; 0 |

admitted=false; census known; no b/x.

### 5. VOID20 entry
As H12a.

### 6/9. Counts and retransmissions
Faults 1 total. Retransmission 1 (input 3): silenced by rej-hist ("keeps only the refutation history needed to silence its identical retransmission"); ordinary counter observation C0 unchanged → nothing. Retransmission 2 (input 5): rej-hist survives the valid replacement (D40 "Retain rejected-control refutation history across ... later recording of a different final terminal claim"); identity check applies first, so the conflicting-final-claim rule is never reached. Neither is a DATA envelope, so §2/§4 duplicate_identity (internal breach, nonzero exit) does not apply.

---

## H13 — census {D,H}: `EXIT_H18@L, S(19), D20@P0{final}, S(25), TERM_H18, AD_D6, S(30)` (detection 20, C0)

### 1. Inputs and positions
| # | Input | Role |
|---|---|---|
| 1 | DATA(L,1,18,H,EXIT) cookie-bearing, bound to H | staged; eligible without disposition (D40) |
| 2a-2d | BOUNDARY(P0,19,1), (P1,19,1), (E,19,1), (L,19,2) | 2d: W=19, releases EXIT_H18 and F18 (completed release evidence for H) |
| 3 | DATA(P0,2,20,D,TRANS,final attachment) | staged; held by D's missing ADMIT; w[P0]=20 |
| 4a-4d | BOUNDARY(P0,25,3), (P1,25,2), (E,25,2), (L,25,3) | 4d: W=25, D20 eligible but held by missing-ADMIT dependency → batch and frontier held (no F24) |
| 5 | TERM_H18 names L/1, 18 | final MATCHED for H; H discharge x_H=18 commits (backed by previously completed EXIT release) |
| 6 | AD_D6 (e=6, valid) | admitted; releases D20[U] (fresh-record anchor b_D=20); closing attempt: F24, checkpoint, END(21) |
| 7a-7d | BOUNDARY(P0,30,4), (P1,30,3), (E,30,3), (L,30,4) | 7d: F29 |

### 2. Stream
`EXIT_H18 → F18 → TRANS_D20[prior_interval_certified=false] → F24 → F29`. Rejected publications: none.

### 3. Timeline
`OPEN` → `EXIT_H18` (2d) → [checkpoint C0] → `F18` (2d) → `D20[U]` (6) → `F24` (6) → [checkpoint C0] → `END(21)` (6; commitment instant of b_D=20 and closure; x_H already committed at 5) → [checkpoint C0] → `F29` (7d). No VOID, no O!.

### 4. State
| After | Epoch | Latch | D (admitted, b_D) | H (claim, x_H) | Accessor H L/1 | Pending L+Q | Census | Gap, C_close/P_close |
|---|---|---|---|---|---|---|---|---|
| start | active | clear | false, – | none | NONE | 2+0=2 | known | open |
| 1 | active | clear | false | none | NONE | 2 | known | open |
| 2a-2c | active | clear | false | none | NONE | 2 | known | open; W=0 |
| 2d | active | clear | false | none (EXIT released, F18) | NONE | 2 | known | open |
| 3 | active | clear | false, held D20 | none | NONE | 2 | known | open |
| 4a-4c | active | clear | false | none | NONE | 2 | known | open; W=19 |
| 4d | active | clear | false; prospective bound 20 (not committed) | none | NONE | 2 | known | open; held |
| 5 | active | clear | false | MATCHED 18 H unrefuted; x_H=18 | MATCHED(18,H) | 1+0=1 | known | open |
| 6 | active | clear | true, b_D=20 committed by END | x_H=18 | MATCHED(18,H) | 0+0=0 | known | closed; C_close=20, P_close=21 |
| 7a-7d | active | clear | same | same | MATCHED(18,H) | 0 | known | closed; historical/current C_close=20, P_close=21 |

D has no EXIT and no terminal disposition at any point. H admitted=false throughout. No faults.

### 10. TERM_H18 and AD_D6
- Before TERM_H18: persisted `OPEN, EXIT_H18, F18`; reader: startup gap open. After TERM_H18: same prefix (no publication: D still unresolved, so closure is impossible). TERM may use H's completed EXIT release (attempt at 2d finished with F18 and its checkpoint without fault; D40 "supplies x_i=exit_ns only when the corresponding EXIT passes strict release eligibility and the complete release attempt commits without fault"; D41 "outside a release attempt this permits the current terminal-discharge candidate backed by its previously completed EXIT release"). It may not use D's prospective bound 20 or the W=25 cutoff from the held batch (D3 "Prospective bounds used for validation supply no committed proof or permission to close the startup gap"; D41 "never prospective bounds or readiness state left by a held or failed release").
- Before AD_D6: prefix `OPEN, EXIT_H18, F18`, pending 1. AD_D6 removes the hold; validation sees D20 as the first released TRANS of the final attachment → route (a), b_D=20, [U]. Closure test: census complete (fence 5), b_D=20, x_H=18, no unresolved disposition → C_close=max(5,20,18)=20; completeness W=25 > 20; D20 is an uncertified fresh-record anchor at exactly C_close → P_close=21 (D42). Order per D41: DATA, FRONTIER, checkpoints, END last.
- Prefix immediately before END: `OPEN, EXIT_H18, F18, D20[U], F24` — reader still sees the open gap. Prefix including END: GAP_CLOSE endpoint field 21, reserved field 0; reader: [boot_start,21) unavailable (instant 20 included, so any window containing 20 is unavailable); windows starting at ≥21 eligible under the other rules.

---

## Control H6b — `D20@P0, TERM_D19, RA, EXIT_D18@L, S(25)` (TERM names L/1, claims 19)

### 1. Inputs
1 DATA(P0,1,20,D,TRANS): staged, missing-ADMIT hold, w[P0]=20. 2 TERM_D19: rejected on delivery (retained TRANS at 20 > 19; R-REJ), FAULT #1, latch, D20 disposed. 3 RA accepted (e1). 4 DATA(L,1,18,D cookie,EXIT): independently bound; no fault ("Reconciling that EXIT and retained attributable record timestamps against an already refuted terminal claim produces no additional discovery fault ..., including when the claim was rejected on delivery"); staged. 5 BOUNDARY(P0,25,2), (P1,25,1), (E,25,1), (L,25,2): 5d W=25 → EXIT_D18 released, F24 (detection 22).

### 2. Stream
`VOID20[C0,unknown_evidence] → EXIT_D18 → F24`.

### 3. Timeline
`OPEN → VOID20 → [RA] → EXIT_D18 → [checkpoint C0] → F24`. No END (latch; D unresolved; no disposition).

### 4. State
admitted false; claim none throughout; rej-hist {TERM_D19} from input 2; accessor L/1 NONE throughout; no b/x (no disposition; latch); pending 1; census known; gap open. Epoch active → suspended (2) → active (3 onward). Payloads: 1 (D20) → 0 → 0 → 1 (EXIT) → 0 after release.

### 5. VOID20 entry
Suspended, latch, accessor NONE, rej-hist {TERM_D19}, prefix `OPEN`, [50,60) Unavailable, no revocation.

### 6. Counts
Faults 1; storage none; D20 payload disposed; EXIT published normally (not uncovered, not abandoned).

---

## Control H6c — `TERM_D19, D20@P0, RA, TERM_D21` (same prospective EXIT L/1; no EXIT DATA)

### 1. Inputs
1 TERM_D19: no retained contradicting evidence → recorded final MATCHED claim (L/1, 19, D). 2 DATA(P0,1,20,D,TRANS): attributable TRANS after claimed time refutes the recorded claim (D40) → FAULT #1, latch, D20 disposed, claim retained as refuted. 3 RA accepted. 4 TERM_D21 (detection 22): conflicts in terminal time with the retained (refuted) final claim → FAULT #2 (R-CONFLICT), rejected, retained claim unchanged.

### 2. Stream
`VOID20[C0,unknown_evidence] → VOID22[C0,unknown_evidence]`.

### 3. Timeline
`OPEN → VOID20 → [RA] → VOID22`.

### 4. State
| After | Epoch | Latch | Claim | Rej-hist | Accessor L/1 | Pending |
|---|---|---|---|---|---|---|
| 1 | active | clear | MATCHED 19 D unrefuted | none | MATCHED(19,D) | 1 |
| 2 | suspended | set | MATCHED 19 refuted | none | MATCHED(19,D) refuted | 1 |
| 3 | active | set | same | none | same | 1 |
| 4 | suspended | set | same (not replaced) | TERM_D21 rejected (conflict) | MATCHED(19,D) refuted | 1 |

admitted false; no b/x; census known; gap open.

### 5. VOID entries
VOID20: suspended, latch, refuted claim 19, prefix `OPEN`. VOID22: suspended, latch, same claim, prefix `OPEN, VOID20`. No revocation (nothing committed). [50,60) Unavailable at both.

### 6. Counts
Faults 2. Storage none. D20 disposed.

---

## Control H11e — `AD_D19, EXIT_D18@L, TERM_D17{names L/1}, TERM_D18`

### 1. Inputs
1 AD_D19 recorded (admitted true). 2 DATA(L,1,18,D cookie,EXIT): independently bound EXIT x=18 < e=19; ADMIT recorded before → retain admission, refute its proof, FAULT #1 (D36), latch, EXIT payload disposed. 3 TERM_D17: R-REJ first (EXIT stamped 18 ≠ 17) — D36 "Apply Decision 40's rejection-on-delivery rule before recording a separately false terminal control or applying this same-contradiction suppression" → FAULT #2, recorded nothing. 4 TERM_D18: only contradiction is the already reported D36 one → recorded as final MATCHED, refuted, no discharge, no report.

### 2. Stream
`VOID20[C0,unknown_evidence] → VOID20[C0,unknown_evidence]`.

### 3. Timeline
`OPEN → VOID20 (input 2) → VOID20 (input 3)`.

### 4. State
| After | Epoch | Latch | ADMIT proof | Claim | Rej-hist | Accessor L/1 |
|---|---|---|---|---|---|---|
| 1 | active | clear | unrefuted | none | none | NONE (not named) |
| 2 | suspended | set | refuted | none | none | NONE |
| 3 | suspended | set | refuted | none | {TERM_D17} | NONE |
| 4 | suspended | set | refuted | MATCHED 18 D refuted | {TERM_D17} | MATCHED(18,D) refuted |

admitted true from 1; no b/x; pending 1; census known; gap open.

### 5. VOID entries
First: proof refuted, no claim, accessor NONE, prefix `OPEN`. Second: plus rej-hist {TERM_D17}, prefix `OPEN, VOID20`. No revocation. [50,60) Unavailable.

### 6. Counts
Faults 2. Storage none. EXIT payload disposed.

---

## Control H11f — `AD_D19, EXIT_D18@L{no cookie}, TERM_D18, finish(40,C0)`

Inputs 1-3 exactly as H11g inputs 1-3 (AD recorded; undecided EXIT staged and held, obligation created; TERM_D18 establishes the D36 contradiction → refuted MATCHED recorded, ADMIT proof refuted, FAULT #1, latch, EXIT disposed). Input 4 finish(40,C0): obligation satisfied by the refuted MATCHED; disposed payload with disposition recorded → no unfinished-record fault; no finish fault.

Stream: `VOID20[C0,unknown_evidence]`. Timeline: `OPEN → VOID20`. VOID entry: as H11g's. Faults 1; storage none.

Accounting (item 7): pending payload records 1 after EXIT, 0 after TERM and at finish; unfinished_records 0; unresolved inventory D (L=1); obligation satisfied at TERM. Accessor MATCHED(18,D) refuted both after TERM and at finish; fault total 1 at both. Nothing justifies a finish fault.

---

## Targeted mutants (design only; none built or executed)

| Row | Mutant | Distinguishing input and assertion |
|---|---|---|
| H11g | Retains the disposed EXIT as unfinished after REARM | finish(40): contract unfinished_records=0, no VOID40, fault total 1; the mutant reports 1 unfinished record and/or a finish FAULT |
| H11g | Treats RA's continuing-attachment evidence as restoring the refuted ADMIT proof | after RA: ADMIT proof still refuted, latch set, pending L=1, no END after S(26) |
| H11g | Treats a refuted MATCHED as no disposition | finish: no missing-disposition VOID40; accessor MATCHED |
| H12a | Retains the rejected TERM_D19 as a final claim | TERM_D18: no fault, accessor MATCHED(18) unrefuted; mutant faults on conflict |
| H12a | Imports an enable/termination contradiction without ADMIT | TERM_D18: claim unrefuted, fault total 1 |
| H12b | Lets the rejected TERM_D19 satisfy the disposition obligation, then rejects TERM_D18 as a duplicate/conflict | TERM_D18: accessor MATCHED(18,D), no fault; finish no fault |
| H12c | Counts the disposed EXIT payload as discharging the obligation (no finish fault) | finish: VOID40[C0,unknown_evidence], accessor NONE, fault total 2, unfinished_records 0 |
| H12c | Counts the disposed payload as an unfinished record (extra fault) | finish: exactly one finish fault, unfinished_records 0 |
| H12d | Ignores retained TRANS evidence for a control naming a prospective EXIT (records TERM_D19) | TERM_D19: FAULT, accessor NONE; TERM_D21: no fault, accessor MATCHED(21); total 1 (mutant gives 2 and MATCHED 19) |
| H6c (guard) | Suppresses conflicts against a claim validly recorded before its refutation (treats it as rejected) | TERM_D21: VOID22 required, accessor stays MATCHED(19) refuted; mutant records 21 silently |
| H12e | Checks a later final-claim conflict before rejected-retransmission identity | input 5: contract no fault (total 1); mutant emits a second VOID20 |
| H12e | Drops rej-hist when TERM_D18 is recorded | same input 5 assertion |
| H12e | Treats a control retransmission as duplicate DATA (internal breach) | input 3: no VOID, no nonzero exit |
| H13 | Loses H's terminal discharge during D's missing-ADMIT hold (requires the TERM to ride a new completed release, or discards it because the current batch is held) | TERM_H18: x_H=18, pending 1; AD_D6: END endpoint 21 (mutant: pending stays 2, no END) |
| H13 | Uses D's held prospective bound to close at TERM_H18 | no END before AD_D6; prefix after TERM = `OPEN, EXIT_H18, F18` |
| H13 | Omits the D42 increment | END endpoint 21, not 20 |

---

## Decision 41 END-publisher result requirement

The startup publisher must report END failure only when no complete END record is reader-visible ("The startup publisher reports failure for END only when no complete END is reader-visible; a reader-visible END is committed, and a later failure is a separate observation"). Two cases:
- Failed END publication (no complete reader-visible END): the closing attempt fails; it commits none of its candidate bounds, discharges or closure; the fault sets the latch; at VOID entry the epoch is suspended, candidates uncommitted, and every view and the persisted reader agree the gap is still open under its existing OPEN; earlier DATA/FRONTIER of that attempt remain observable under ordinary VOID rules; no rollback OPEN.
- Committed reader-visible END followed by a separate failure observation: the END is committed (including when the process stops before updating in-memory state). The later failure is its own observation with its own consequences (VOID, latch for future commitments); it is not a failed closing attempt and does not uncommit the closure. The latch "alone neither reopens a closed startup gap nor changes its endpoint". Only independently attributable evidence contradicting a committed proof (D43) revokes it, via a persisted revoked OPEN; an unrelated later fault does not.

---

## UNRESOLVED

None needed to derive these executions. One reading was chosen where the text is explicit only indirectly, noted for comparison: (a) H12c's finish-time missing-disposition FAULT is published as VOID40[C0,unknown_evidence] by §4 "Emit VOID for each separately delivered fault report, including during suspension"; (b) in H13 the discharge x_H=18 commits at TERM_H18 (a non-closing control) because D40's discharge conditions (completed EXIT release attempt plus unrefuted matching disposition) are both met then; END only commits closure and b_D. Agreement with another answer set does not establish implementation or platform qualification.

## After the one-sentence clarification 9b (a fresh answerer; the certification marks of H13, D03, D02a, Q01)

# prior_interval_certified markings — H13, D03, D02a, Q01 (contract-only answer, opus)

Sources read: CONTRACT.md, CONTRACT-coverage-a2-v1.md, oracle-discovery/ROWS.md, oracle-discovery/TASK.md. Nothing else.
All four rows use mode N (no override to Q is stated; ROWS.md: "Mode is N unless overridden"). No fault arises in any of the four executions, so the certification latch is never set and every listed record is successfully published.

General marking rule used throughout (CONTRACT §3):
> "Discovery is a distinct route: its TRANS/MARKER outputs carry `prior_interval_certified=false` for incoming intervals required to remain uncertified by Decisions 37 and 43, and true for incoming intervals certified under those decisions, subject to ordinary validity rules. In particular, Decision 37(a)'s first successfully released final-attachment TRANS/MARKER carries false, including when its DATA was received and held before matching ADMIT arrived. Compute these markings in validated release order, not from admission status at DATA receipt."

## H13

Inputs: `EXIT_H18@L, S(19), D20@P0{final}, S(25), TERM_H18, AD_D6, S(30)`.

TRANS/MARKER records: only D20 (H has no numerical records; EXIT_H18 is a lifecycle record, not TRANS/MARKER).

- D20 is held at S(25) waiting for ADMIT (Decision 36: "A valid control may arrive after dependent numerical DATA; those records and their release frontier wait for matching ADMIT or accepted ABANDON_DISCOVERY"). It becomes releasable at the AD_D6 evaluation (W=25 > 20; release is evaluated after every input, §1/§3), at latest at S(30); the marking is the same either way.
- No B/CONFIRM is supplied, so the route is 37(a). D20 is the first successfully released TRANS/MARKER of D's final attachment.
- **D20: false.** Decided by §3: "Decision 37(a)'s first successfully released final-attachment TRANS/MARKER carries false, including when its DATA was received and held before matching ADMIT arrived", and Decision 37(a): "Mark its incoming interval uncertified, including duration, CPU and completed-wait count."
- No fault: H's matched TERM agrees with its EXIT (no false or conflicting facts); AD_D6 (e=6) is not later than any D EXIT (none exists). So nothing withholds publication.

Undecided: no.

## D03

Inputs: `D15(X→0,3)@P1, D12(Y→X)@P0, S(16), AD_D8`.

- Both records are staged (D15 arrives first, from P1; D12 from P0 — different sources and timestamps, so no clock regression, seal violation or tie). They wait for ADMIT; after AD_D8 (W=16) both release in key order D12, D15.
- **D12: false.** It is first in validated release order of the final attachment's records. Decided by §3: "Decision 37(a)'s first successfully released final-attachment TRANS/MARKER carries false, including when its DATA was received and held before matching ADMIT arrived. Compute these markings in validated release order, not from admission status at DATA receipt." (The earlier arrival of D15 is irrelevant under the last sentence.)
- **D15: true.** Decision 37(a): the first record "establishes coherent post-record state" and sets `b_i` to its timestamp (12); D15's incoming interval [12,15) is based on that state, is not an earlier-attempt record and is not the first record, so it is not "required to remain uncertified" by Decision 37 and is "certified under those decisions" (§3 sentence quoted above). Consistency check: Decision 42 says "an interval uncertified by provenance and ending after that bound contradicts it and reports FAULT(unknown_evidence,d,C)"; D15 ends at 15 > b_i=12, so marking it uncertified would contradict the bound the same release commits. The bound commits in this attempt (census {D} complete at 5, C_close=12, D12 anchor at 12 gives P_close=13, completeness strictly beyond 12 holds at W=16; END published last).

Note: Decision 37(a) has no sentence as literal as 37(b)'s "Subsequent intervals based on this seed require no discovery-specific first-record exclusion"; the true marking rests on §3's "true for incoming intervals certified under those decisions" plus 37(a)'s "establishes coherent post-record state" and the Decision 42 contradiction rule. I treat this as decided: no sentence supports false for D15.

Undecided: no.

## D02a

Inputs: `AD_D13, B_D12, S(12), S(13), D15(X→0,3)@P0, S(16)`.

- Route 37(b): quiet witness t=12, e=13, matching ADMIT recorded first. Decision 37(b): "Accept the lower bound only after matching ADMIT, qualified source completeness strictly beyond `t`, completed failure accounting, and reconciliation…". S(12) does not supply completeness strictly beyond 12; S(13) does, so the bound b_i=12 commits (with closure: C_close=max(5,12)=12, no uncertified record at 12, P_close=12, END) before D15 arrives.
- Only TRANS/MARKER record: D15, released at S(16).
- **D15: true.** Decided by Decision 37(b): "On successful commitment set `b_i=t`. Subsequent intervals based on this seed require no discovery-specific first-record exclusion. Records preceding the baseline, or belonging to earlier attempts, remain uncertified and must not modify the confirmed baseline." D15 neither precedes the baseline nor belongs to an earlier attempt, so by §3 it carries true ("true for incoming intervals certified under those decisions").

Undecided: no.

## Q01

Inputs: `AD_D13, B_D12, D15(X→0,3,cpu=1)@P0, S(16)` (mode N; the Q prefix is a row family, not mode Q — D13e is the row that states "Mode Q").

- Same route as D02a, but no boundary precedes D15. W stays 0 until the fourth constituent of S(16); at BOUNDARY(L,16) W=16, completeness strictly beyond t=12 holds, and D15's release and the quiet bound's commitment (closure attempt, END last) occur in the same release attempt.
- **D15: true.** Decided by the same Decision 37(b) sentences: "Subsequent intervals based on this seed require no discovery-specific first-record exclusion. Records preceding the baseline, or belonging to earlier attempts, remain uncertified…", with §3's "true for incoming intervals certified under those decisions" and "Compute these markings in validated release order". D15 is not a record preceding the baseline or from an earlier attempt, so no sentence requires it to remain uncertified.
- Same-attempt point considered: D15 is published before END, which is what commits the bound (Decision 41: "Successful END publication commits the affected bound, discharge and closure"). No sentence makes the marking depend on the bound having committed before the record's publication; the only commitment-dependent uncertified rule is Decision 43's latch case ("without a previously committed valid numerical bound their incoming numerical intervals remain uncertified"), which applies only after a fault has set the latch, and none occurs here. So this does not leave the marking undecided.

Undecided: no.

## Summary

| execution | records in publication order | marking of each | undecided |
|---|---|---|---|
| H13 | D20 | D20=false | no |
| D03 | D12, D15 | D12=false, D15=true | no |
| D02a | D15 | D15=true | no |
| Q01 | D15 | D15=true | no |
