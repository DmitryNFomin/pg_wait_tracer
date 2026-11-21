# Independent answer set (opus): merge-v1 delta 7

Written from the amended contract, ROWS.md and the TASK alone. Two packets: the first on delta 7 as first written (28 executions H1a-H10b), the second, by a fresh answerer, on the text after settlements 7b and 7c (34 selected executions including H11a-H11c).

## First packet (delta 7 as first written; rows H1a-H10b)

# merge-v1 delta 7: independent answers for rows H1a-H10b (contract text only)

## 0. Conventions used in every section

- Sources `{P0,P1,E,L}`; per-source positions start at 1. `S(w)` = `BOUNDARY(P0,w)`, `BOUNDARY(P1,w)`, `BOUNDARY(E,w)`, `BOUNDARY(L,w)`, each with that source's next position, and release is evaluated after each one. Starting from cutoff 0 (initially, and after any accepted REARM, Decision 27), `W=min(w[s])` stays 0 until the L boundary. So any output from an `S(w)` happens at its fourth input (L). I do not repeat this in the rows.
- `OPEN0` = the startup hard gap opened at `boot_start`=0 before startup coverage is advertised (Decision 41). It is persisted before the first row input. `END(x)` = startup GAP_CLOSE with endpoint `P_close=x` (Decision 42). `ROPEN` = revoked startup OPEN at `boot_start` (Decision 43).
- `F<n>[C]` = `FRONTIER(n,C)`, `VOIDd[C,reason]` = `VOID(d,C,reason)`. `[U]` = `prior_interval_certified=false`.
- Defaults: D=(pid 30,g=2^63) is installed and independently bound; the census `{D}` is complete at fence 5; mode N; C0; detection time 20 (22 after RA). After `K` every observation carries Cr. A lifecycle EXIT of D carries D's discovery cookie unless the row says `{no cookie}`.
- Fault diagnostics: each fault adds +1 to its own reason's diagnostic count and adds nothing to the storage vector (§4, Decision 43). The only storage-vector change in these rows is the real `wait_read_fail=1` that `K` observes.
- "pending L/Q" = unresolved inventoried lifetimes / unresolved numeric observations (Decision 41). Unless stated, `discovery_abandoned_records=0` and uncovered-record consumption = 0. There is no epoch invalidation and no nonzero exit unless stated.
- Order inside one release attempt that closes the gap: supporting DATA first, then `END`, then FRONTIER. Decision 41 requires supporting records to be published before closure commits, and a discharge or bound commits only when the attempt, including its FRONTIER, completes (Decision 40, the 2026-11-20 clarification, and Decision 41's final paragraph). Placing `END` before the FRONTIER follows the description in row N14. The contract text does not fix END against FRONTIER (AMBIGUITY A3).
- Fate of records staged before a VOID or received while suspended, once a REARM is accepted, is not decided (AMBIGUITY A1). Reading **R-drop**: VOID discards the staged suffix, and arrivals during suspension are dropped once checked (Decision 43, "before dropping its payload during suspension"). Reading **R-keep**: staged records survive and may be consumed after REARM (Decision 40, "staging ... while suspended ... any subsequent consumption requires an independently valid REARM"). Only H3d, H6a and H6b depend on it.

---

## H1a — `TERM_D18, EXIT_D18@L, S(19), AD_D19`

1. TERM_D18: a matched terminal claim naming L position 1 at 18 is recorded. No output.
2. EXIT_D18@L (L o=1): staged at W=0.
3. S(19) (P0 o1, P1 o1, E o1, L o2): at L, W=19. The eligible batch is EXIT_D18. A cookie-bearing, independently bound EXIT of a never-admitted lifetime may be published without ADMIT (Decision 40). It is published, the discharge candidate x=18 completes with its attempt, and closure is computed: census complete, D discharged, no unresolved disposition, `C_close=max(5,18)=18`, completeness 19>18. No uncertified discovery TRANS/MARKER exists at 18, and a lifecycle EXIT alone does not trigger the increment (Decision 42), so `P_close=18`.
   Output: `EXIT_D18 → END(18) → F18[C0]`. The discharge x_D=18 and the closure commit.
4. AD_D19: e=19 is later than the independently bound EXIT at 18. Decision 36 says this is "contradictory provenance in either arrival order. Reject an ADMIT contradicted on delivery without recording admission, route or baseline." The latch is set first (Decision 43), then FAULT(unknown_evidence,20,C0).
   Output: `VOID20[C0,unknown_evidence]`. The revocation clause covers only proofs "supported by the contradicted proof". The discharge rests on TERM+EXIT, so nothing is revoked (see A7).

- **Stream:** `EXIT_D18 → END(18) → F18[C0] → VOID20[C0,unknown_evidence]`.
- **Faults:** AD_D19 adds unknown_evidence +1.
- **Final state:** admitted=false (never recorded). Discharge x=18 stands. No bound. Pending L0/Q0, total 0. Census known complete. Latch set. Epoch e0 suspended.
- **Gap history:** OPEN0, END(18), then VOID. No ROPEN.

## H1b — `AD_D19, EXIT_D18@L, S(20)` (no TERM)

1. AD_D19: no EXIT is known yet, so the ADMIT is valid and recorded. admitted=true (numerical route). No bound.
2. EXIT_D18@L (o1): an independently bound cookie EXIT at 18 is earlier than the recorded e=19. Decision 36: "If the ADMIT was validly recorded before the EXIT established the contradiction, retain the historical admission fact and refute its proof". The observation that establishes the contradiction reports the FAULT. The latch is set first.
   Output: `VOID20[C0,unknown_evidence]`. Nothing has committed, so there is no ROPEN ("Refuting a candidate that never committed does not itself publish a revoked OPEN").
   Reading: the EXIT arrival establishes the contradiction. If it were found only at batch validation (the L boundary of S(20)), the stream would be identical, because the fault precedes release (A8).
3. S(20): the epoch is suspended. No release and no FRONTIER.

- **Stream:** `VOID20[C0,unknown_evidence]`.
- **Final state:** admitted=true, proof refuted. No bound or discharge (no TERM, latch set). Pending L1/Q0. Census known (unknown_evidence is outside the lifecycle-loss class). Latch set. Suspended.
- **Gap history:** OPEN0 only.
- **Counts:** abandonment 0, uncovered 0.

## H1c — `TERM_D18, EXIT_D18@L, S(19), AD_D18`

Steps 1-3 match H1a: `EXIT_D18 → END(18) → F18[C0]`, with x=18 and closure committed.

4. AD_D18: e=18 equals the EXIT timestamp. "Equality of enable and EXIT timestamps is not this contradiction." It is a valid ADMIT after the EXIT was released: "recorded as its final disposition and is no fault for that order" (Decision 36). Decision 41 adds: "once its discharge has committed, a later valid ADMIT ... is recorded and commits no bound."

- **Stream:** `EXIT_D18 → END(18) → F18[C0]`.
- **Final state:** admitted=true. Discharge 18. No bound. Pending 0. Census known. Latch clear. Epoch active. No fault.
- **Gap history:** OPEN0, END(18).

## H1d — `AD_D18, TERM_D18, EXIT_D18@L, S(19)`

1. AD_D18 is recorded: admitted=true, numerical route.
2. TERM_D18 is recorded as a matched claim.
3. EXIT_D18 (L o1) is staged. e=18 equals the EXIT time, so there is no contradiction.
4. S(19) at L, W=19: EXIT_D18 is released. No bound has committed, so the matched discharge is the first resolving proof to commit (Decision 41). Decision 38 says an admitted lifetime "remains pending until a qualifying record actually releases or a matching terminal discharge is accepted". `C_close=18`, `P_close=18`.
   Output: `EXIT_D18 → END(18) → F18[C0]`.

- **Final state:** admitted=true. x=18. Pending 0. Census known. Latch clear. Active. No fault.
- **Gap history:** OPEN0, END(18).

## H2a — `K, RA, S(25), AD_D6, D26@P0{final}, S(27)`

1. K=CHECK(20,Cr): the vector changed from C0. The latch is set before publication. Output: `VOID20[Cr,counter_change]`. e0 is suspended. Census stays known, because wait_read_fail is outside the lifecycle-loss class (Decision 31).
2. RA=REARM(21,Cr,A/B): D has no recorded ADMIT, so RA carries no D attachment. It is accepted and starts e1 with baseline Cr and cutoffs 0.
3. S(25) at L, W=25: the batch is empty and the FRONTIER advances. Output: `F24[Cr]`.
4. AD_D6 (22,Cr): a valid startup ADMIT recorded under the latch, with no latch fault (Decision 43). admitted=true, and the missing-ADMIT hold is removed.
5. D26@P0 (P0 o2): w[P0]=26. W=25, so D26 is held.
6. S(27) (P0 o3, P1 o2, E o2, L o2), at L W=27, D26 eligible. **Not decided (A2).**
   - **R2-i:** the recorded ADMIT lets D's records release "under the ordinary validation, completeness, publication and explicit-REARM rules" (Decision 43). Output: `D26[U] → F26[Cr]`. D26 is the fresh-record anchor, but "no new discovery bound ... commits" under the latch, so its incoming interval is uncertified.
   - **R2-ii:** recovery requires fresh attachment proofs (§4), RA preceded the ADMIT, and the H-row RA definition includes D's attachment only "when D has a valid recorded ADMIT". D26 therefore lacks attachment evidence in e1, which is unknown evidence at batch validation. Output: `VOID22[Cr,unknown_evidence]`, no D26, no F26. Nothing was committed, so there is no ROPEN.
   - **R2-iii (weak):** AD after REARM counts as "recovery-time admission", which Decision 43 forbids. Against this, Decision 43 also says that recording a startup ADMIT under the latch "is completion of the recorded startup disposition, not a new recovery attachment". So this reading is disfavoured.

- **Stream (R2-i):** `VOID20[Cr,counter_change] → F24[Cr] → D26[U] → F26[Cr]`.
- **Stream (R2-ii):** `VOID20[Cr,counter_change] → F24[Cr] → VOID22[Cr,unknown_evidence]`.
- **Final state:** admitted=true. No bound or discharge. Pending L1. Census known. Latch set. Gap OPEN0 only. Abandonment 0, uncovered 0.

## H2b — `K, AD_D6, RA, S(25), D26@P0{final}, S(27)`

1. K: `VOID20[Cr,counter_change]`. Latch set, e0 suspended.
2. AD_D6 (20,Cr): recorded during suspension and under the latch. admitted=true.
3. RA: D has a valid recorded ADMIT, so RA includes D's independently qualified continuing attachment. Accepted, e1, baseline Cr.
4. S(25): `F24[Cr]`.
5. D26@P0 (o2): held at W=25.
6. S(27) at L, W=27: D26 is the first released TRANS of the final attachment and is marked [U]. The latch bars the bound (Decision 43). Output: `D26[U] → F26[Cr]`.

- **Stream:** `VOID20[Cr,counter_change] → F24[Cr] → D26[U] → F26[Cr]`.
- **Final state:** admitted=true. No bound. Pending L1. Census known. Latch set. e1 active. Gap OPEN0. Counts 0.

## H2c — `K, AD_D6, D26@P0{final}, S(27)` (no REARM)

1. K: `VOID20[Cr,counter_change]`. Latch set, suspended.
2. AD_D6 is recorded under the latch. admitted=true.
3. D26@P0 (o1): the arrival checks pass. Trusted callbacks are suppressed while suspended, and there are no committed proofs to check it against.
4. S(27): suspended, so no release and no FRONTIER.

- **Stream:** `VOID20[Cr,counter_change]`.
- **Final state:** admitted=true. No bound. Pending L1. Census known. Latch set. Suspended. Gap OPEN0. Counts 0 (no consumption while suspended).

## H2d — `K, RA, AD_D6, AB_D`

1. K: `VOID20[Cr,counter_change]`. Latch set.
2. RA: accepted, e1, without D's attachment (no ADMIT yet).
3. AD_D6 (22,Cr): recorded. admitted=true.
4. AB_D (22,Cr): "An already admitted identity cannot be abandoned". A recorded ADMIT "bars abandonment throughout the capture, including when recorded under the latch" (Decision 45). The abandonment is invalid. Output: `VOID22[Cr,unknown_evidence]`. e1 is suspended. No abandonment is recorded and nothing is revoked.

- **Stream:** `VOID20[Cr,counter_change] → VOID22[Cr,unknown_evidence]`. No FRONTIER, because no boundary arrived in e1.
- **Final state:** admitted=true. Not abandoned. Pending L1. Census known. Latch set. Suspended. Gap OPEN0. Abandoned records 0.

## H2e — `K, RA, ADc6, AB_D`

This matches H2d with a route-(c) ADMIT. ADc6 is recorded under the latch (valid; it asserts its facts). TRY_NW_D6 is not delivered, but route-(c) ADMIT validity does not require that report to have arrived first (compare G2). The bar on abandoning an admitted identity covers any recorded ADMIT. No CN6 arrives, and the latch bars any bound anyway.

- **Stream:** `VOID20[Cr,counter_change] → VOID22[Cr,unknown_evidence]`.
- **Final state:** admitted=true (route c). No bound. Pending L1. Census known. Latch set. Suspended. Gap OPEN0. Counts 0.

## H3a — `D12@P0{final}, ADc6, TRY_NW_D6`

1. D12@P0 (o1): an attributable TRANS of D. It is staged and held for the missing ADMIT.
2. ADc6: "An attributable TRANS/MARKER contradicts a permanent no-wait-word proof" (Decision 43). The retained D12 contradicts the route-(c) claim at delivery, so the ADMIT is rejected without being recorded (Decision 36; Decision 43, "A discovery control rejected for its own invalid facts ... records no accepted claim, admission"). Latch set. Output: `VOID20[C0,unknown_evidence]`. Nothing committed, so no ROPEN.
3. TRY_NW_D6: no ADMIT has been recorded, so Decision 38's late-attempt fault does not apply. It is an adapter observation, not a claim, and it adds no fault. Reading: it is retained as pointer-watch history. See A10 for the alternative.

- **Stream:** `VOID20[C0,unknown_evidence]`.
- **Final state:** admitted=false. No bound. Pending L1. Census known. Latch set. Suspended. D12 never released. Gap OPEN0. Counts 0.

## H3b — `D12@P0{final}, ADc6, PTR_D14(non-NULL)`

Steps 1-2 match H3a: `VOID20[C0,unknown_evidence]`, ADc6 rejected.

3. PTR_D14 (non-NULL): no route-(c) proof was recorded or committed. Decision 38: a qualified non-NULL fire "makes that still-pending lifetime eligible for ordinary Decision 37(a) or 37(b) attachment" and supplies no bound or record. No fault.

- **Stream:** `VOID20[C0,unknown_evidence]`.
- **Final state:** admitted=false. Pending L1. Census known. Latch set. Suspended. Gap OPEN0. Counts 0.

## H3c — `D12@P0{final}, ADc6, BREAK_D`

Steps 1-2 match H3a.

3. BREAK_D: no route-(c) proof exists, and no separate FAULT is supplied. **Not decided (A6).**
   - **R6-i:** no fault. Nothing recorded or committed depends on the pointer continuity. Decision 43's break rule applies "after an accepted Decision 37(c) bound".
   - **R6-ii:** Decision 39 says "A demonstrated observation-continuity violation follows the existing fault rules", and §4 says an "attachment interruption ... remains a counted coverage fault, delivered as FAULT(unknown_evidence)". That gives a second `VOID20[C0,unknown_evidence]` while suspended. No revocation and no census change (no committed 37(c) bound).

- **Stream (R6-i):** `VOID20[C0,unknown_evidence]`.
- **Stream (R6-ii):** `VOID20[C0,unknown_evidence] → VOID20[C0,unknown_evidence]`.
- **Final state:** admitted=false. Pending L1. Census known. Latch set. Suspended. Gap OPEN0.

## H3d — `D12@P0{final}, ADc6, AB_D, RA, D26@P0{earlier}, S(27)`

1. D12@P0 (o1): staged.
2. ADc6: rejected as in H3a. `VOID20[C0,unknown_evidence]`. Latch set, e0 suspended. admitted=false.
3. AB_D (20,C0): accepted. D is inventoried and unambiguously bound with no recorded ADMIT ("An ADMIT rejected before recording does not itself bar abandonment"). The gap is open and the attempts have permanently ended (row override). Acceptance during suspension "neither cancels a fault nor rearms" (Decision 45). No output.
4. RA (21,C0): D has no valid recorded ADMIT, so RA carries only A/B. Accepted, e1, cutoffs 0.
5. D26@P0{earlier} (o2, 22): the arrival checks pass (last P0 DATA was 12, no retained P0 boundary). It belongs to the abandoned stream.
6. S(27) (P0 o3, P1 o1, E o1, L o1) at L, W=27: abandoned records are consumed under ordinary release (Decision 45) and abandonment consumption continues under the latch (Decision 43). D26 is discarded, never published, and counted. The FRONTIER advances.

- **Stream (R-drop):** `VOID20[C0,unknown_evidence] → F26[C0]`. `discovery_abandoned_records=1` (D26). D12 was disposed at VOID and is not counted, because merely received or VOIDed records do not count (Decision 42).
- **Stream (R-keep):** the same, but D12 (t=12, eligible) is also consumed and discarded in the same attempt, so `discovery_abandoned_records=2` (A1).
- **Final state:** admitted=false. Abandoned=true. No bound or discharge. Pending L1. Census known. Latch set. e1 active. Gap OPEN0 (discards lie inside the open gap). Uncovered 0.

## H4a — `AD_D6, TRY_NW_D6, RA, B_D6{e=6}, A26@P0, S(27)`

1. AD_D6 is recorded: admitted=true, numerical route.
2. TRY_NW_D6 arrives after a recorded numerical ADMIT with no qualified non-NULL fire recorded. Decision 38: it "reports FAULT(unknown_evidence,d,C) and is not recorded". It is evidence against the address proof, so the ADMIT proof is refuted, but no committed numerical bound exists to revoke. Latch set. Output: `VOID20[C0,unknown_evidence]`. No ROPEN.
3. RA (21,C0): accepted, e1. Per the H-row definition, D's continuing attachment is included, because its ADMIT was validly recorded (admitted stays true after refutation). No D DATA follows, so this has no effect.
4. B_D6{e=6} (22): a quiet candidate. Confirmation delivery may follow ADMIT (37(b)), and it is not a wait-word attempt report. Reading: it is recorded with no fault, but it supplies no readiness, because the admission proof is refuted and the latch is set (A9).
5. A26@P0 (o1): staged.
6. S(27) (P0 o2, …) at L, W=27. Output: `A26 → F26[C0]`.

- **Stream:** `VOID20[C0,unknown_evidence] → A26 → F26[C0]`.
- **Final state:** admitted=true, proof refuted. No bound. Pending L1. Census known. Latch set. e1 active. Gap OPEN0. Counts 0.

## H4b — `AD_D6, TRY_NW_D6, RA, B_D6{e=6}, TRY_NW_D6, S(27)`

Steps 1-4 match H4a.

5. The second TRY_NW_D6 (22,C0) is late again: "including a repeat of an earlier report" (Decision 38). Decision 43 does not suppress "the late-attempt reports expressly required by Decision 38". Output: `VOID22[C0,unknown_evidence]`. e1 is suspended. There is no bound to revoke.
6. S(27): suspended, no output.

- **Stream:** `VOID20[C0,unknown_evidence] → VOID22[C0,unknown_evidence]`.
- **Faults:** unknown_evidence +2 in total.
- **Final state:** admitted=true. No bound. Pending L1. Census known. Latch set. Suspended. Gap OPEN0. Counts 0.

## H5a — `B_D6{e=6,α2}, K, RA, D14@P0{earlier}`

1. B_D6: a quiet candidate (t=6, e=6, attachment α2) recorded before any ADMIT, which quiet candidates may do. No ADMIT ever arrives in this row.
2. K: `VOID20[Cr,counter_change]`. Latch set. The candidate survives as evidence (Decision 43).
3. RA (21,Cr): no D attachment. Accepted, e1.
4. D14@P0{earlier} (o1, 22, Cr): an earlier-attempt (α1) record. Its uncertified interval ends at 14, after the proposed bound t=6. Decision 42: this "contradicts it and reports FAULT(unknown_evidence,d,C)". Decision 43: "the first observation establishing its contradiction faults". **Not fully decided (A8).**
   - **R8-i (arrival):** `VOID22[Cr,unknown_evidence]`, e1 suspended. Never committed, so there is no ROPEN.
   - **R8-ii (only at reconciliation or release):** D14 never becomes eligible (no boundaries) and the candidate never reaches commitment, so there is no second VOID.

- **Stream (R8-i):** `VOID20[Cr,counter_change] → VOID22[Cr,unknown_evidence]`.
- **Stream (R8-ii):** `VOID20[Cr,counter_change]`.
- **Final state:** admitted=false. No bound. The candidate is refuted (R8-i) or merely unready (R8-ii). Pending L1. Census known. Latch set. Suspended (R8-i) or e1 active (R8-ii). Gap OPEN0. Counts 0.

## H5b — `K, RA, B_D6{e=6,α2}, D14@P0{earlier}`

1. K: `VOID20[Cr,counter_change]`.
2. RA: accepted, e1.
3. B_D6 (22,Cr): recorded under the latch with no latch fault.
4. D14{earlier}: the same contradiction and the same readings as H5a.

- **Stream:** identical to H5a under each reading: R8-i `VOID20[Cr,counter_change] → VOID22[Cr,unknown_evidence]`, R8-ii `VOID20[Cr,counter_change]`.
- The final state is identical to H5a. The refutation does not depend on order.

## H6a — `TERM_D19, D20@P0, RA, EXIT_D18@L, S(25)` (TERM names L o1 and claims 19)

1. TERM_D19: a matched claim recorded for the prospective L o1, terminal time 19.
2. D20@P0 (o1): an attributable TRANS stamped 20, after the claimed terminal time 19. Decision 40: it "refutes that terminal match in either arrival order". Latch set. Output: `VOID20[C0,unknown_evidence]`. Nothing committed, so no ROPEN. D20 is retained as a per-lifetime summary.
3. RA (21,C0): accepted, e1, A/B only. The P0 retained boundary is 20.
4. EXIT_D18@L (o1, 22): its timestamp 18 differs from the claimed 19, but "A terminal claim is refuted once; subsequent observations contradicting that already refuted claim report nothing ... including after REARM." No fault (A12). It is a cookie-bearing EXIT and eligible for ordinary publication.
5. S(25) at L, W=25:
   - **R-drop:** release `EXIT_D18 → F24[C0]`. No discharge (the claim is refuted and the latch is set).
   - **R-keep:** D20 (no ADMIT, no AB) is still staged and held for the missing ADMIT. Decision 36 says "those records and their release frontier wait", and §3 says a hold "holds the batch and its frontier". Whether EXIT_D18 can release as a prefix (`EXIT_D18 → F19[C0]`) or the whole batch is held (no output) is not decided. A further reading is that D20 has no attachment proof in e1, which would be unknown evidence and a VOID22 (A1).

- **Stream (R-drop):** `VOID20[C0,unknown_evidence] → EXIT_D18 → F24[C0]`.
- **Final state:** admitted=false. No discharge. Pending L1. Census known. Latch set. e1 active. Gap OPEN0. Counts 0.

## H6b — `D20@P0, TERM_D19, RA, EXIT_D18@L, S(25)`

1. D20@P0 (o1): staged and held for the missing ADMIT.
2. TERM_D19: contradicted at delivery by the retained D20 ("in either arrival order"). Output: `VOID20[C0,unknown_evidence]`. Latch set. The claim is refuted, or rejected without recording. Either way it supplies no discharge, and the refutation history is retained.
3. Steps 3-5 match H6a, with the same readings.

- **Stream (R-drop):** `VOID20[C0,unknown_evidence] → EXIT_D18 → F24[C0]`. The final state is identical to H6a.

## H6c — `TERM_D19, D20@P0, RA, TERM_D21` (both name the same prospective L position; no EXIT)

1. TERM_D19 is recorded.
2. D20 refutes it. Output: `VOID20[C0,unknown_evidence]`. Latch set.
3. RA (21,C0): e1.
4. TERM_D21 (22): a separately delivered matched claim for the same EXIT with a different terminal time. D20 (20<21) does not contradict it. **Not fully decided (A5).**
   - **R5-i:** Decision 40 says "a separately delivered conflicting claim retains its own fault consequences" and "conflicting disposition or identity claims for the same EXIT report FAULT(unknown_evidence,d,C)". The final disposition is retained after refutation, and the conflicting control does not replace it. Output: `VOID22[C0,unknown_evidence]`, e1 suspended.
   - **R5-ii:** no rule names a matched-versus-matched conflict without a committed discharge. On this reading, TERM_D21 is recorded or ignored without a fault, and the latch bars any discharge anyway.

- **Stream (R5-i):** `VOID20[C0,unknown_evidence] → VOID22[C0,unknown_evidence]`.
- **Stream (R5-ii):** `VOID20[C0,unknown_evidence]`.
- **Final state:** admitted=false. No discharge. Pending L1. Census known. Latch set. Gap OPEN0. No ROPEN (nothing committed). Counts 0.

## H7a — `AD_D6, D12@P0{final}, D14@P0{earlier}, A20@P0{requires x}, S(40)` (x unavailable)

1. AD_D6 is recorded: admitted=true.
2. D12 (P0 o1), D14 (P0 o2) and A20 (P0 o3) are staged.
3. S(40) (P0 o4, P1 o1, E o1, L o1) at L, W=40: the eligible batch is D12, D14 and A20. A20's dependency is unavailable, which is a hold. But §3 says: "Complete all eligible-batch validation decidable from available evidence, including reconciliation of prospective fresh-record bounds against the batch's records, before returning held; an established contradiction faults at once". The prospective fresh-record bound is 12 (D12). D14 is an earlier-attempt interval ending at 14 > 12, which contradicts it (Decision 42). Latch set. Output: `VOID20[C0,unknown_evidence]`. No DATA and no FRONTIER. The bound was only prospective, so there is no ROPEN.
   Reading: the fault is established at the L boundary of S(40). An alternative places it at D14 arrival; the stream and final state are the same (A8).

- **Stream:** `VOID20[C0,unknown_evidence]`.
- **Final state:** admitted=true, proof contradicted. No bound. Pending L1. Census known. Latch set. Suspended. Gap OPEN0. Counts 0.

## H7b — `AD_D6, A10@P1{requires x}, D12@P0{final}, D14@P0{earlier}, S(40)`

This matches H7a. Positions are A10 P1 o1, D12 P0 o1, D14 P0 o2, and S(40) at P0 o3, P1 o2, E o1, L o1. A10, which precedes D's records in key order, is held. Validation of the whole batch still finds that D14 contradicts the prospective bound 12 before the hold is returned.

- **Stream:** `VOID20[C0,unknown_evidence]`. The final state is identical to H7a.

## H8a — `AD_D6, B_D6{e=6}, S(19), TRY_NW_D6` (no non-NULL fire)

1. AD_D6 is recorded (numerical). B_D6 is recorded as a quiet candidate (t=6, e=6).
2. S(19) at L, W=19 > t: the matching ADMIT and the reconciliation are complete, so b_D=6 commits (37(b)). Closure: census complete, D resolved, `C_close=max(5,6)=6`. There is no uncertified discovery record at 6, so `P_close=6`. The batch is empty.
   Output: `END(6) → F18[C0]`.
3. TRY_NW_D6: a late attempt report after a numerical ADMIT with no qualified non-NULL fire recorded. It faults, is not recorded, and "revokes any committed numerical lower bound" (Decision 38). Decision 43 sequence: set the latch, revoke b_D, persist ROPEN, update every public view, then publish `VOID20[C0,unknown_evidence]`. Census completeness is not invalidated: "These added refutation cases do not themselves invalidate census completeness".

- **Stream:** `END(6) → F18[C0] → ROPEN → VOID20[C0,unknown_evidence]`.
- **Public views at VOID-callback entry:** admitted=true. Committed bound none (b=6 revoked). Discharge none. Pending L1/Q0, total 1. Census known complete. Latch set. Startup gap open (ROPEN governs). Fault diagnostics already include this unknown_evidence (see A4 on suspension visibility).
- **Persisted prefix at callback entry:** `OPEN0, END(6), F18[C0], ROPEN`. The persisted reader reports the startup gap open from `boot_start` with no later END, so every window from 0 is unavailable. The historical END(6) and F18 remain observable but are superseded. Evaluating the saved prefix alone, with no later publication, gives the same result, and it agrees with the accessors.
- **Final state:** as at entry. Epoch e0 suspended. No invalidation (no frontier disproved). Counts 0.

## H8b — `NW_D6, S(19), PTR_D12(non-NULL)`

1. NW_D6 = the TRY_NW_D6 history, then AD_D6{route=c} (admitted=true), then CONFIRM_NO_WAIT_WORD (a valid candidate after its ADMIT).
2. S(19) at L, W=19 > e=6: b_D=6 commits (37(c)). `C_close=6`. 37(c) supplies no uncertified record, so `P_close=6`. Output: `END(6) → F18[C0]`.
3. PTR_D12 (non-NULL), observed at 20: "A non-NULL pointer fire after an accepted Decision 37(c) bound reports FAULT(unknown_evidence,d,C)". The bound and base coverage are revoked, the gap reopens, D returns to pending, and "Census completeness must be re-established" (Decision 43). Sequence: latch, revoke, persist ROPEN, update views, then `VOID20[C0,unknown_evidence]`.

- **Stream:** `END(6) → F18[C0] → ROPEN → VOID20[C0,unknown_evidence]`.
- **Public views at VOID-callback entry:** admitted=true (route c). Bound revoked. Pending L1/Q0, total 1. Census **unknown**. Latch set. Gap open.
- **Persisted prefix at entry:** `OPEN0, END(6), F18[C0], ROPEN`. The reader reports the startup gap open from 0. The prefix alone gives the same result.
- **Epoch invalidation:** none. PTR supplies no transport DATA or boundary, so it does not disprove F18 (A11).
- **Final state:** e0 suspended. Counts 0.

## H9a — `PTR_D5(non-NULL), AD_D6, B_D6{e=6}, S(8), TRY_NW_D6`

1. PTR_D5 (non-NULL): a qualified fire recorded for the pending D. It makes D eligible for 37(a)/(b) attachment (Decision 38). No fault.
2. AD_D6 is recorded (numerical). B_D6 is recorded as a candidate.
3. S(8) at L, W=8 > 6: b_D=6 commits. `C_close=6`, `P_close=6`. Output: `END(6) → F7[C0]`.
4. TRY_NW_D6 is late after a numerical ADMIT, so it faults and is not recorded. A qualified non-NULL fire is recorded for D, so the clause about evidence against the address proof does not apply and nothing is revoked (Decision 38). Latch set. Output: `VOID20[C0,unknown_evidence]`.

- **Stream:** `END(6) → F7[C0] → VOID20[C0,unknown_evidence]`.
- **Final state:** admitted=true. b=6 committed and unrefuted. Pending 0. Census known. Latch set. The gap stays closed at END(6), because the latch does not reopen it. Suspended. No ROPEN. Counts 0.

## H9b — `PTR_D5(non-NULL), AD_D6, B_D6{e=6}, S(8), WAIT_D6`

This matches H9a. WAIT_D6 is "a wait-word attempt report delivered after an ADMIT", so it faults and is not recorded. A non-NULL fire is recorded, so it revokes nothing.

- **Stream:** `END(6) → F7[C0] → VOID20[C0,unknown_evidence]`. The final state is identical to H9a.

## H10a — `TERM_D18, EXIT_D18@L{no cookie}, S(19), TERMU_D18{cookie=0; names that EXIT; claims final unresolved}`

1. TERM_D18: a matched final disposition delivered before the EXIT. It applies when the EXIT is consumed, which is then not held (Decision 40).
2. EXIT_D18{no cookie} (L o1): staged.
3. S(19) at L, W=19: the matched cookie-less EXIT is released and published. x_D=18 commits. `C_close=18`, `P_close=18`. Output: `EXIT_D18 → END(18) → F18[C0]`.
4. TERMU_D18: "An unresolved disposition for the same EXIT after a matched disposition has supplied a committed discharge likewise faults and revokes that discharge" (Decision 40). The cookie-zero numeric-only acceptance also fails (PID 30 has an inventory entry). One observation gives one FAULT and no new disposition. The retained final disposition stays matched, and there is no second consumption of the EXIT. Sequence: latch, revoke x_D, persist ROPEN, update views, then `VOID20[C0,unknown_evidence]`.

- **Stream:** `EXIT_D18 → END(18) → F18[C0] → ROPEN → VOID20[C0,unknown_evidence]`.
- **Views at VOID entry:** admitted=false. Discharge revoked. Pending L1/Q0. Census known (an added refutation case). Latch set. Gap open.
- **Persisted prefix:** `OPEN0, END(18), EXIT_D18, F18, ROPEN`. The reader reports the gap open from 0.
- **Counts:** uncovered consumption 0 (the EXIT was consumed as matched and published). Abandonment 0. No unresolved disposition is recorded. Suspended.

## H10b — J numeric defaults: `X_4, A6@P0, S(8), J_U4, TERMU_U4`

Start: numeric observation of PID 50, no inventory entry. Pending L0/Q1. Census unknown, because an unresolved numeric observation prevents completeness. Gap OPEN0.

1. X_4 (L o1): a cookie-less EXIT of PID 50, which matches an unresolved numeric observation. It needs a final disposition and is held until one arrives (Decision 40).
2. A6@P0 (o1): staged.
3. S(8) (P0 o2, P1 o1, E o1, L o2) at L, W=8: X_4 is held for its undecided terminal, and A6 is after it in key order. Nothing releases. Hold.
4. J_U4: J resolution. It creates the inventory lifetime U and resolves the numeric observation, so pending becomes L1/Q0 and the census is known complete at fence 5. It applies a matched disposition to X_4 (Decision 31 J). Release evaluation follows: X_4 is published, A6 is released, x_U=4 commits with the attempt, the census is complete, U is discharged, `C_close=max(5,4)=5`, W=8>5, `P_close=5`. Output: `EXIT_U4 → A6 → END(5) → F7[C0]`.
5. TERMU_U4: an unresolved disposition after a matched disposition that supplied a committed discharge. It faults and revokes. The cookie-zero acceptance conditions are also false (PID 50 now has an inventory entry and no unresolved numeric observation). That gives one FAULT(unknown_evidence,20,C0) and no new disposition. Sequence: latch, revoke x_U, persist ROPEN, update views, then `VOID20[C0,unknown_evidence]`.

- **Stream:** `EXIT_U4 → A6 → END(5) → F7[C0] → ROPEN → VOID20[C0,unknown_evidence]`.
- **Views at VOID entry:** U pending, L1/Q0, total 1. Census known: the established identity and numeric resolution are preserved, and this is a refutation case. Discharge revoked. Latch set. Gap open.
- **Persisted prefix:** `OPEN0, EXIT_U4, A6, END(5), F7, ROPEN`. The reader reports the gap open from 0.
- **Counts:** uncovered 0. Suspended.

---

## Existing revocation rows (strengthened publication ordering)

I did not see the delta's list of rows or their earlier expected outputs, so I cannot compare them. From the text alone, the following rows have a fault that revokes a committed proof: G2b, the G3 revocation variant, the G4 variants, G4b, P2, the committed variants of R3, R10's second variant, N5, and N6 if its PTR(NULL) faults (it does not). For each of them, Decision 43 now requires this order: latch, every revocation from the observation coalesced into one ROPEN, ROPEN persisted, public views updated, then the VOID callback.

So in each such row the earlier stream gains `ROPEN` immediately before its VOID. In a two-fault observation (Decision 43), the ROPEN comes before the first (arrival) VOID. DATA, FRONTIER and final outcomes are unchanged. Decision 43 says the prior END and DATA stay observable, and the VOID count is unaffected. A row whose earlier answer placed the revoked OPEN after the VOID, or left it out, must change to this order.

---

## AMBIGUITIES

- **A1. Staged or suspended-arrival DATA across VOID and REARM (H3d, H6a, H6b).** Decision 40 contemplates "staging" while suspended and "subsequent consumption" after REARM. §4 "discard the tentative merge suffix" and Decision 43 "dropping its payload during suspension" suggest disposal instead. The consequences:
  - H3d: `discovery_abandoned_records` is 1 (R-drop) or 2 (R-keep).
  - H6a/H6b under R-keep: D20 is held for the missing ADMIT. It is undecided whether EXIT_D18 releases as a prefix with F19, the whole batch is held, or D20 faults for lacking an e1 attachment proof.
- **A2. H2a.** An ADMIT recorded after RA, when RA carried no D attachment, either lets D26 release (`D26[U] → F26[Cr]`) or leaves D26 without fresh attachment evidence (`VOID22[Cr,unknown_evidence]`). A third, disfavoured reading treats it as forbidden recovery-time admission.
- **A3. END against FRONTIER inside one release attempt (H1a, H1c, H1d, H8a, H8b, H9a, H9b, H10a, H10b).** I used END before FRONTIER (row N14's description). The normative text only requires supporting publication before closure commits, and the attempt (with its FRONTIER) to complete before commitments.
- **A4. Suspension at VOID-callback entry (H8a, H8b, H10a, H10b).** Decision 43 lists the views that must already reflect the revocation (commitments, pending, gap, census, latch) but not epoch suspension. Whether accessors show "suspended" inside the callback is not stated.
- **A5. H6c.** A second, conflicting matched claim for the same EXIT, after the first claim was refuted and with no committed discharge, either faults (`VOID22`) or does not.
- **A6. H3c.** A demonstrated pointer-continuity break with no recorded or committed route-(c) proof either faults under Decision 39 and §4 ("attachment interruption"), giving a second VOID20, or does not.
- **A7. H1a.** I read the ADMIT rejected for e > EXIT as revoking nothing, because the discharge is not "supported by the contradicted proof". A reading under which the rejected ADMIT is "independently attributable evidence contradicting a committed terminal discharge" (Decision 43) would add ROPEN before VOID20 and leave D pending.
- **A8. Which observation establishes a contradiction (H1b, H5a, H5b, H7a, H7b).** It is either the arriving record or eligible-batch and commitment reconciliation. The streams are identical for H1b, H7a and H7b. For H5a/H5b, arrival gives `VOID22[Cr,unknown_evidence]`, while reconciliation-only gives no second VOID, because D14 never becomes eligible and the candidate never reaches commitment.
- **A9. H4a/H4b.** Whether B_D6, delivered after the admission's address proof was refuted by a late TRY, is a false claim (a fault) or a recorded candidate with no readiness. I read it as no fault.
- **A10. H3a.** Whether TRY_NW_D6, delivered after a rejected ADc6 and contradicted by the retained D12 role evidence, itself faults. I read it as no fault, because it is an observation and not a claim.
- **A11. H8b.** Whether a non-NULL PTR at 12, below the published F18, also invalidates epoch e0. I read it as no, because PTR supplies no transport DATA or boundary.
- **A12. H6a/H6b.** Whether EXIT_D18 in e1, with retained D20 stamped after it, is a new contradiction. I read it as no, because the claim is already refuted and Decision 40's "refuted once" rule applies.

## Second packet (after settlements 7b and 7c; fresh answerer; 34 selected executions)

# merge-v1 delta 7c — independent answers (opus)

Derived from CONTRACT-merge-v1.md (§2-§6, DECISIONs 30-45), CONTRACT-coverage-a2-v1.md, ROWS.md and TASK.md only.

## Conventions used in every section

- Sources P0,P1,E,L; source positions are per source and start at 1; `S(w)` = `BOUNDARY(P0,w)`, `BOUNDARY(P1,w)`, `BOUNDARY(E,w)`, `BOUNDARY(L,w)` in that order, each its own primitive input with its own release evaluation (§5). Qualified perf DATA on Pi also supplies the implicit boundary w=t (§3). Controls (AD, ADc, CN, B, TRY, TERM, TERMU, AB, PTR, BREAK, AOP, CHECK, FAULT, REARM) consume no source position.
- Publication symbols (combined stream): `OPEN0` = plain startup GAP_OPEN from boot_start (=0), published before any input (D41 "Open a hard base-coverage gap at boot_start before advertising startup coverage"); `rOPEN0` = revoked startup OPEN from boot_start (D43); `rbOPEN0` = rollback plain OPEN (D41, "This rollback alone uses a plain OPEN, is not counted as a revoked OPEN"); `END(p)` = startup GAP_CLOSE with endpoint P_close=p, reserved field 0 (D42); `F<n>[C]` = FRONTIER(n,C); `VOID<d>[C,reason]`; DATA by name. "Merge-only stream" = the same list with OPEN/END/rOPEN removed.
- Only successful publications are listed as publications; rejected writer calls are listed separately (§3 "A rejected writer call is not a DATA output").
- State tuple after each primitive input: `adm` (public admitted, D43), `proof` (admission-proof status), claims, `b`, `x`, `P=L+Q` (pending lifetimes + pending numeric observations), census, latch, epoch, gap. "commit" means commitment (D41: END publication is not commitment).
- Defaults (G/H/R/A rows): D installed and independently bound, census {D} complete (known) at fence 5, C0, detection 20, mode N; initial state adm=false, no claims, b=x=none, P=1+0, census complete, latch off, epoch e0 active (baseline C0), gap open (history: OPEN0). Default-row counts: abandonment (`discovery_abandoned_records`) 0, uncovered-consumption 0, epoch invalidations none, nonzero exit none, unless stated.
- A VOID's latch: "the first fault requiring VOID permanently latches discovery certification off before fault publication" (D43). On every fault the epoch suspends first and staged unpublished DATA/TEXT are discarded (§4); such disposal is neither abandonment nor uncovered consumption (§4 last sentences).
- None of the faults below is an internal breach, so no execution exits nonzero. No late DATA has t <= a published frontier, so no execution has an epoch invalidation (§4 "If offending late DATA has t<= an already published frontier ... mark ... invalid"; D43 "Apply ordinary epoch invalidation additionally when the evidence disproves a published frontier" — control-only refutations disprove no frontier).

---

## H1a — `TERM_D18, EXIT_D18@L, S(19), AD_D19`

1. TERM_D18: matched terminal claim (x claim 18, names EXIT_D18's L position 1) recorded. No output. State: adm=false, b=x=none, P=1+0, census complete, latch off, e0 active, gap open.
2. EXIT_D18@L (L o1, cookie-bearing): staged; W=0. No output.
3. S(19): P0 o1, P1 o1, E o1: W stays 0, no output. L o2: W=19. Eligible EXIT_D18 (cookie-bearing EXIT of a never-admitted lifetime is publishable without ADMIT/ABANDON/disposition, D40 penultimate paragraph). Discharge candidate x=18 (TERM unrefuted + this release attempt). Closure: census complete, every entry discharged in this attempt, no unresolved disposition; C_close=max(5,18)=18; W=19 > 18; no uncertified discovery TRANS/MARKER at 18, "a lifecycle EXIT alone does not trigger the increment" (D42) so P_close=18. Order (D41): `EXIT_D18 → END(18) → F18[C0]`. Checkpoint C0 = baseline; attempt completes → x=18 commits, closure commits. State: x=18 committed (resolving proof committed), P=0+0, census complete, gap closed at 18.
4. AD_D19: e=19 > independently bound EXIT 18 → contradictory provenance; "Reject an ADMIT contradicted on delivery without recording admission, route or baseline" (D36). This input reports FAULT(unknown_evidence,20,C0). Refute recorded claims: no recorded ADMIT; TERM_D18 refuted (D36 "Refute each recorded claim among them: ... any recorded matched terminal claim for that EXIT"). "rejection of the ADMIT does not preserve the disputed discharge" → revoke x=18 and dependent coverage (D43): persist `rOPEN0`, invalidate cached answers relying on END(18), D back to pending. Census not reset ("These added refutation cases do not themselves invalidate census completeness", D43). Sequence: suspend e0, latch on, rOPEN0 published, then `VOID20[C0,unknown_evidence]`. unknown_evidence diagnostic +1; no storage counter change.
   - At VOID entry: e0 suspended; latch on; adm=false; TERM_D18 refuted; x revoked; P=1+0; census complete; gap open; cache invalidated for END(18); persisted history OPEN0, END(18), rOPEN0; persisted reader: gap open from 0 — agrees with public view.
   - Final: adm=false, no admission claim, b=none, x=none (revoked), P=1+0, census complete, latch on, e0 suspended, gap open.

Combined: `OPEN0, EXIT_D18, END(18), F18[C0], rOPEN0, VOID20[C0,unknown_evidence]`. Merge-only: `EXIT_D18, F18[C0], VOID20[C0,unknown_evidence]`. Abandoned 0, uncovered 0. No invalidation, no exit.

## H1b — `AD_D19, EXIT_D18@L, S(20)`

1. AD_D19: valid on delivery (no EXIT known); recorded. adm=true, proof valid (route a pending), b=none, P=1+0, latch off, e0 active, gap open.
2. EXIT_D18@L (L o1): cookie-bearing EXIT of D is independently bound (G-row note; D40 "A discovery cookie is usable only when it is bound to this exact kernel lifetime"). e=19 > 18 → "contradictory provenance in either arrival order"; "If the ADMIT was validly recorded before the EXIT established the contradiction, retain the historical admission fact and refute its proof. The observation establishing the contradiction reports FAULT" (D36). Reconciled during this primitive input (D42). No committed proof → no revoked OPEN. Suspend, latch on, staged EXIT_D18 discarded (not consumed), `VOID20[C0,unknown_evidence]`.
   - At VOID entry: e0 suspended, latch on, adm=true, proof refuted, no terminal claim, P=1+0, census complete, gap open; persisted OPEN0 only → reader says open; agrees. No cache invalidation needed (no END ever).
3. S(20): four boundaries accepted (w=20 each) while suspended; no FRONTIER (§4). No output.

Combined: `OPEN0, VOID20[C0,unknown_evidence]`. Merge-only: `VOID20[C0,unknown_evidence]`. Abandoned 0, uncovered 0.

## H1c — `TERM_D18, EXIT_D18@L, S(19), AD_D18`

1-3 as H1a: `EXIT_D18 → END(18) → F18[C0]` at BOUNDARY(L,19); x=18 committed, gap closed at 18, P=0+0.
4. AD_D18: "Equality of enable and EXIT timestamps is not this contradiction" (D36). "A valid ADMIT delivered after that lifetime's lifecycle EXIT has been released is recorded as its final disposition and is no fault for that order" (D36); "once its discharge has committed, a later valid ADMIT ... is recorded and commits no bound" (D41). adm=true, proof valid, b=none, x=18 stays, gap stays closed, latch off, e0 active. No output.

Combined: `OPEN0, EXIT_D18, END(18), F18[C0]`. Merge-only: `EXIT_D18, F18[C0]`. No fault, no VOID.

## H2a — `K, RA, S(25), AD_D6, D26@P0{final}, S(27)`

1. K = CHECK(20,Cr): counter_change. Suspend e0, latch on, `VOID20[Cr,counter_change]`. wait_read_fail is outside the lifecycle-loss class → census stays complete (D31). At VOID entry: suspended, latch on, adm=false, P=1+0, census complete, gap open, persisted OPEN0 → open; agree.
2. RA = REARM(21,Cr,A/B proofs; no D proof since D has no recorded ADMIT): accepted (suspended). Epoch e1 active, baseline Cr, cutoffs 0; subsequent detection 22.
3. S(25): W=25 at L o1 → `F24[Cr]` (advances; no earlier frontier). Latch blocks any END. P=1+0, gap open.
4. AD_D6: startup ADMIT after accepted REARM; per H-row note it supplies continuity through RA and the current observation (D43 "its independently qualified proof supplies the current epoch's attachment evidence"). Recorded under latch without fault (D43). adm=true, proof valid; missing-ADMIT hold removed. No bound can commit (latch).
5. D26@P0{final} (P0 o2): staged; W=min(26,25,25,25)=25 → not eligible.
6. S(27): P0/P1/E: W stays 25. L: W=27. Eligible D26, D's first released final-attachment record; published with incoming interval uncertified ("without a previously committed valid numerical bound their incoming numerical intervals remain uncertified", D43): `D26[prior_interval_certified=false] → F26[Cr]`. b not committed (latch). No END.

Final: adm=true, proof valid, b=none, x=none, P=1+0, census complete, latch on, e1 active, gap open.
Combined: `OPEN0, VOID20[Cr,counter_change], F24[Cr], D26[U], F26[Cr]`. Merge-only the same minus OPEN0. Storage counter wait_read_fail=1 (observed, not caused by merge). Abandoned 0, uncovered 0.

## H2b — `K, AD_D6, RA, S(25), D26@P0{final}, S(27)`

1. K: `VOID20[Cr,counter_change]`, latch on, e0 suspended, census complete. VOID entry as H2a.
2. AD_D6: valid; recorded during suspension under the latch, no fault (D43 "A valid subsequent control is recorded without a fault caused by the latch"). adm=true, proof valid.
3. RA: accepted; includes D's continuing attachment (D has a valid recorded ADMIT, H-row RA note). e1 active, baseline Cr, detection 22.
4. S(25): `F24[Cr]`.
5. D26@P0: staged, W=25.
6. S(27): at L: `D26[U] → F26[Cr]`; no bound (latch), no END.

Final as H2a. Combined: `OPEN0, VOID20[Cr,counter_change], F24[Cr], D26[U], F26[Cr]`.

## H3a — `D12@P0{final}, ADc6, TRY_NW_D6`

1. D12@P0 (P0 o1): staged (waits for ADMIT/ABANDON, D36); P0 w=12; W=0.
2. ADc6 (route-(c) ADMIT, e=6): retained D12 is an attributable TRANS after e; "An attributable TRANS/MARKER contradicts a permanent no-wait-word proof" (D43); arriving controls are reconciled against retained evidence during the input (D42); "Reject an ADMIT contradicted on delivery without recording admission, route or baseline" (D36). FAULT(unknown_evidence,20,C0): suspend, latch on, D12 discarded, `VOID20[C0,unknown_evidence]`. No commitment → no rOPEN. At VOID entry: suspended, latch on, adm=false, no route-(c) claim, P=1+0, census complete, gap open, persisted OPEN0 → open; agree.
3. TRY_NW_D6: no ADMIT ever recorded → "retained as an observation of its supplied physical history, not as an accepted admission or permanence claim" (D43); no fault, no output.

Final: adm=false, b=x=none, P=1+0, census complete, latch on, e0 suspended, gap open.
Combined: `OPEN0, VOID20[C0,unknown_evidence]`. Abandoned 0, uncovered 0.

## H3b — `D12@P0{final}, ADc6, PTR_D14(non-NULL)`

1-2 as H3a (ADc6 rejected, `VOID20[C0,unknown_evidence]`).
3. PTR_D14(non-NULL): "If no accepted, unrefuted route-(c) claim exists, the observation alone supplies no discovery-refutation fault"; "a rejected admission does not establish a required accepted attachment proof" (D43). No fault; the fire is retained pointer history (D38: makes the pending lifetime eligible for 37(a)/(b) attempts). No output.

Combined: `OPEN0, VOID20[C0,unknown_evidence]`. Final state as H3a.

## H3c — `D12@P0{final}, ADc6, BREAK_D`

1-2 as H3a.
3. BREAK_D: same D43 rule (continuity break refutes only a still-unrefuted accepted route-(c) claim; none exists; not a §4 continuity violation because no accepted attachment proof). No fault, no output.

Combined: `OPEN0, VOID20[C0,unknown_evidence]`. Final state as H3a.

## H3d — `D12@P0{final}, ADc6, AB_D, RA, D26@P0{earlier}, S(27)`

1-2 as H3a: D12 staged then discarded by fault handling; ADc6 rejected; `VOID20[C0,unknown_evidence]`; latch on.
3. AB_D: identity inventoried and bound, no ADMIT ever recorded ("An ADMIT rejected before recording does not itself bar abandonment", D45), gap open, attempts ended (H-row note) → accepted (during suspension; "Acceptance neither cancels a fault nor rearms", D45). D12 is already disposed → not counted. Abandoned 0.
4. RA: REARM(21,C0,A/B proofs; no D proof). e1 active; retained P0 boundary 12 and last P0 DATA ts 12; detection 22.
5. D26@P0{earlier} (P0 o2): arrival valid (26 > 12). Belongs to the abandoned stream; staged for consumption; W=0.
6. S(27): at L W=27: D26 passes validated release consumption, never published; `discovery_abandoned_records`=1. `F26[C0]` published. Latch → no END; abandonment supplies no b (D45).

Final: adm=false, b=x=none, P=1+0, census complete, latch on, e1 active, gap open.
Combined: `OPEN0, VOID20[C0,unknown_evidence], F26[C0]`. Abandoned 1 (validated consumption of D26); D12 = fault disposal (not counted). Uncovered 0.

## H4a — `AD_D6, TRY_NW_D6, RA, B_D6{e=6}, A26@P0, S(27)`

1. AD_D6: numerical ADMIT recorded; adm=true, proof valid.
2. TRY_NW_D6: after a recorded numerical ADMIT with no qualified non-NULL fire → "reports FAULT(unknown_evidence,d,C) and is not recorded ... is also evidence against the admission's address proof and revokes any committed numerical lower bound" (D38). Admission proof refuted (adm stays true, D43); no committed b → no rOPEN. Suspend, latch on, `VOID20[C0,unknown_evidence]`. VOID entry: suspended, latch on, adm=true, proof refuted, P=1+0, census complete, gap open, persisted OPEN0 → open; agree. TRY contributes no retained history (D43).
3. RA: accepted, e1 active, baseline C0, detection 22.
4. B_D6{e=6}: "A quiet candidate whose own witness facts are valid is recorded without a fault after its matching admission proof was refuted ... neither restores admission validity nor supplies commitment readiness" (D43). Recorded, no fault, no output.
5. A26@P0 (P0 o1): staged, W=0.
6. S(27): at L W=27 → `A26 → F26[C0]`. No discovery commitment.

Final: adm=true, proof refuted, quiet candidate recorded (no readiness), b=none, P=1+0, census complete, latch on, e1 active, gap open.
Combined: `OPEN0, VOID20[C0,unknown_evidence], A26, F26[C0]`.

## H5a — `B_D6{e=6,α2}, K, RA, D14@P0{earlier}`

1. B_D6: quiet candidate recorded before ADMIT (allowed: "Unlike a quiet-baseline candidate, this control cannot precede its matching ADMIT", D37(c)); proposes fixed t=6 (D42 "a quiet candidate already proposes its fixed timestamp even before matching ADMIT"). adm=false.
2. K: `VOID20[Cr,counter_change]`, latch on, e0 suspended; census complete. VOID entry: P=1+0, gap open, reader open; agree.
3. RA: e1 active, baseline Cr, detection 22; candidate survives ("Validly recorded claims, including quiet-baseline candidates, survive VOID, REARM and the certification latch", D43).
4. D14@P0{earlier}: arrival valid. Earlier-attempt interval ends at 14 > proposed bound 6 → contradiction (D42 "an interval uncertified by provenance and ending after that bound contradicts it and reports FAULT"), established at this primitive input. Candidate refuted; nothing committed → no rOPEN. Suspend e1, D14 discarded, `VOID22[Cr,unknown_evidence]`. VOID entry: e1 suspended, latch on, candidate refuted, P=1+0, census complete, gap open; reader open; agree.

Combined: `OPEN0, VOID20[Cr,counter_change], VOID22[Cr,unknown_evidence]`. Abandoned 0, uncovered 0.

## H5b — `K, RA, B_D6{e=6,α2}, D14@P0{earlier}`

1. K: `VOID20[Cr,counter_change]`, latch on.
2. RA: e1 active, detection 22.
3. B_D6: recorded under the latch without fault; proposes t=6.
4. D14@P0{earlier}: same contradiction as H5a → `VOID22[Cr,unknown_evidence]`, candidate refuted, no rOPEN.

Combined: `OPEN0, VOID20[Cr,counter_change], VOID22[Cr,unknown_evidence]`. Final as H5a.

## H6a — `TERM_D19, D20@P0, RA, EXIT_D18@L, S(25)`

1. TERM_D19: matched claim (time 19, names L o1) recorded.
2. D20@P0 (P0 o1): attributable TRANS stamped 20 > claimed 19 → "refutes that terminal match in either arrival order" (D40). FAULT(unknown_evidence,20,C0); TERM_D19 refuted; nothing committed → no rOPEN. Suspend, latch on, D20 discarded, `VOID20[C0,unknown_evidence]`. VOID entry: suspended, latch on, adm=false, P=1+0, census complete, gap open; reader open; agree.
3. RA: e1 active, baseline C0, detection 22; retained P0 boundary 20.
4. EXIT_D18@L (L o1): arrival valid. Its mismatch with refuted TERM_D19 (time) and with D20: "Reconciling that EXIT and retained attributable record timestamps against an already refuted terminal claim produces no additional discovery fault" (D40). Staged (cookie-bearing, publishable without ADMIT).
5. S(25): at L W=25 → `EXIT_D18 → F24[C0]`. No discharge (TERM refuted; latch). No END.

Final: adm=false, TERM_D19 refuted, x=none, P=1+0, census complete, latch on, e1 active, gap open.
Combined: `OPEN0, VOID20[C0,unknown_evidence], EXIT_D18, F24[C0]`. Uncovered 0 (cookie-bearing EXIT published normally).

## H6b — `D20@P0, TERM_D19, RA, EXIT_D18@L, S(25)`

1. D20@P0: staged (no ADMIT), P0 w=20, W=0.
2. TERM_D19: reconciled against retained D20 (20 > 19) → refuted on delivery (D40 "in either arrival order"; D42 reconcile arriving control). FAULT(unknown_evidence,20,C0), suspend, latch on, D20 discarded, `VOID20[C0,unknown_evidence]`. Refutation history retained (D40 "including when the claim was rejected on delivery and only its refutation history was retained"); no discharge possible either way. VOID entry as H6a.
3-5 as H6a: RA → e1; EXIT_D18 no extra fault; `EXIT_D18 → F24[C0]`; no discharge.

Combined: `OPEN0, VOID20[C0,unknown_evidence], EXIT_D18, F24[C0]`.

## H6c — `TERM_D19, D20@P0, RA, TERM_D21`

1. TERM_D19 recorded. 2. D20 refutes it → `VOID20[C0,unknown_evidence]`, latch on, D20 discarded. 3. RA: e1 active, detection 22.
4. TERM_D21: conflicts in terminal time with the retained final claim → "reports FAULT(unknown_evidence,d,C), including when the retained claim is refuted and no discharge ever committed. Reject the conflicting control without replacing or restoring the retained claim" (D40). Suspend e1, `VOID22[C0,unknown_evidence]`. No committed discharge → no rOPEN. VOID entry: e1 suspended, latch on, TERM_D19 refuted (retained), TERM_D21 rejected, P=1+0, census complete, gap open; reader open; agree.

Combined: `OPEN0, VOID20[C0,unknown_evidence], VOID22[C0,unknown_evidence]`.

## H7a — `AD_D6, D12@P0{final}, D14@P0{earlier}, A20@P0{requires x}, S(40)`

1. AD_D6 recorded; adm=true, proof valid.
2. D12@P0{final} (o1): staged; merely staged fresh-record DATA proposes no bound yet (D42).
3. D14@P0{earlier} (o2): arrival valid; staged.
4. A20@P0{requires x} (o3): staged.
5. S(40): P0/P1/E: W=0. L o1: W=40; eligible {D12,D14,A20}. A20's dependency x is unavailable → hold, but "Complete all eligible-batch validation decidable from available evidence, including reconciliation of prospective fresh-record bounds against the batch's records, before returning held; an established contradiction faults at once" (§3). Prospective bound b=12 (D12, first final-attachment record in validated order); D14's earlier-attempt interval ends at 14 > 12 → contradiction (D42). FAULT(unknown_evidence,20,C0) at BOUNDARY(L,40). Prospective bound supplies no commitment → no rOPEN. Suspend, latch on, D12/D14/A20 discarded, `VOID20[C0,unknown_evidence]`. No FRONTIER ever. VOID entry: suspended, latch on, adm=true, b=none, P=1+0, census complete, gap open; reader open; agree. Admission-proof status after this: see UNDECIDED U2.

Combined: `OPEN0, VOID20[C0,unknown_evidence]`. Abandoned 0, uncovered 0.

## H11a — `AD_D19, EXIT_D18@L, TERM_D18`

1. AD_D19 recorded; adm=true.
2. EXIT_D18@L: contradiction e=19 > 18 established; ADMIT proof refuted, historical admission retained (D36); FAULT → suspend, latch on, EXIT discarded, `VOID20[C0,unknown_evidence]`. No committed proof → no rOPEN. VOID entry: suspended, latch on, adm=true, proof refuted, P=1+0, census complete, gap open; reader open; agree.
3. TERM_D18: "A matched terminal control for that same independently bound EXIT, first recorded after this enable/EXIT contradiction was reported, is recorded as refuted, supplies no discharge and adds no report solely for that contradiction" (D36). No fault, no output.

Final: adm=true, proof refuted, TERM_D18 recorded-refuted, x=none, P=1+0, census complete, latch on, e0 suspended, gap open.
Combined: `OPEN0, VOID20[C0,unknown_evidence]`.

## H11b — H1a, then `D20@P0{final}`

1-4 as H1a (ends with `rOPEN0, VOID20[C0,unknown_evidence]`; AD_D19 rejected; TERM_D18 refuted; x revoked; latch on; suspended).
5. D20@P0{final}: arrival valid (P0 retained boundary 19, 20 >= 19). Contradiction with TERM_D18 already refuted → "subsequent observations contradicting that already refuted claim report nothing solely for that contradiction" (D40); no ADMIT claim exists (rejected); no committed proof to check. No fault; payload disposed during suspension (not consumption). No output.

Combined: `OPEN0, EXIT_D18, END(18), F18[C0], rOPEN0, VOID20[C0,unknown_evidence]`. Final as H1a.

## H11c — `K, RA, S(25), AD_D6 (no continuity proof), D26@P0{final}, S(27)`

1. K: `VOID20[Cr,counter_change]`, latch on.
2. RA: e1 active (A/B proofs only), detection 22.
3. S(25): `F24[Cr]`.
4. AD_D6: "A historical startup ADMIT whose original admission facts are valid is recorded even when evidence extending that attachment through REARM and the current observation is missing; recording it does not establish the missing current-epoch continuity" (D43). adm=true, historical proof valid, current-epoch continuity absent. No fault now.
5. D26@P0: staged; W=25.
6. S(27): P0/P1/E: W stays 25. L: W=27 → D's first eligible-batch validation: "Missing or contradicted continuity reports FAULT(unknown_evidence,d,C) at that lifetime's first eligible-batch validation, before publishing any part of that batch" (D43). Suspend e1, D26 discarded, `VOID22[Cr,unknown_evidence]`; no F26. No rOPEN. VOID entry: e1 suspended, latch on, adm=true, b=none, P=1+0, census complete, gap open; reader open; agree.

Combined: `OPEN0, VOID20[Cr,counter_change], F24[Cr], VOID22[Cr,unknown_evidence]`. Admission proof: historical facts not refuted (no false claim); continuity missing.

## G4 (variant 1) — `TERM_D18, EXIT_D18@L, S(19), TERM_D19{names the same EXIT}`

1-3 as H1a: `EXIT_D18 → END(18) → F18[C0]`; x=18 committed; gap closed at 18.
4. TERM_D19: conflicts in terminal time with the retained claim supporting a committed discharge → "faults and the discharge is revoked"; "Reject the conflicting control without replacing or restoring the retained claim" (D40). FAULT(unknown_evidence,20,C0): suspend, latch on, revoke x=18 (D43): `rOPEN0`, invalidate cached answers relying on END(18), D pending; census not reset. Then `VOID20[C0,unknown_evidence]`. VOID entry: suspended, latch on, x revoked, P=1+0, census complete, gap open; persisted OPEN0, END(18), rOPEN0 → open; agree.

Final: TERM_D19 rejected; TERM_D18 retained as final disposition (refuted-label: UNDECIDED U3); x=none; latch on; gap open.
Combined: `OPEN0, EXIT_D18, END(18), F18[C0], rOPEN0, VOID20[C0,unknown_evidence]`.

## G5 — `EXIT_D18@L, S(19), AD_D6, TERM_D18`

1. EXIT_D18@L staged.
2. S(19): at L W=19: `EXIT_D18 → F18[C0]` (eligible without ADMIT/disposition, D40). No discharge yet (no disposition). Gap open, P=1+0.
3. AD_D6: e=6 < 18, no contradiction; "A valid ADMIT delivered after that lifetime's lifecycle EXIT has been released is recorded" (D36). adm=true; no b (no record).
4. TERM_D18: matched disposition backed by the previously completed EXIT release (D41 "outside a release attempt this permits the current terminal-discharge candidate backed by its previously completed EXIT release"); no bound committed first (D41 "the first to commit"). x=18 commits; C_close=18, W=19 > 18, P_close=18; "publish END during that control's evaluation without repeating an already published FRONTIER" → `END(18)`. Gap closed, P=0+0.

Combined: `OPEN0, EXIT_D18, F18[C0], END(18)`. No fault.

## G6 — `TRY_NW_D6, AD_D6, S(19)`

1. TRY_NW_D6: no ADMIT recorded → retained observation (NULL read after e=6, zero fires through fence, permanent role); "retain the contradictory evidence as well" (D43). No fault.
2. AD_D6 (numerical ADMIT, e=6): its required address proof contradicts the retained, independently qualified permanent-role/NULL-pointer history ("The qualification establishes that the identified role cannot acquire or use a wait word", D39; wait-word attempts start only after a non-NULL fire, D38; arriving control reconciled against retained evidence, D42) → contradicted on delivery → rejected without recording (D36), FAULT(unknown_evidence,20,C0). Suspend, latch on, `VOID20[C0,unknown_evidence]`; no rOPEN. VOID entry: suspended, latch on, adm=false, P=1+0, census complete, gap open; reader open; agree.
3. S(19): suspended; boundaries accepted, no FRONTIER.

Combined: `OPEN0, VOID20[C0,unknown_evidence]`. (Inference chain noted in UNDECIDED U4.)

## G7 — `TERM_D18, EXIT_D18@L, S(19), AD_D6, B_D6`

1-3 as H1a: `EXIT_D18 → END(18) → F18[C0]`; x=18 committed, gap closed.
4. AD_D6: valid, recorded, commits no bound (D41). adm=true.
5. B_D6: valid quiet candidate recorded, commits no bound (D41 "a later valid ADMIT or confirmation is recorded and commits no bound"). No output.

Combined: `OPEN0, EXIT_D18, END(18), F18[C0]`. No fault.

## R2 — p=empty, no K: `TERM_D18, D20@P0, EXIT_D18@L, S(25)` (H absent)

1. TERM_D18 recorded.
2. D20@P0: 20 > 18 refutes the match (D40). Nothing committed → no rOPEN. Suspend, latch on, D20 discarded, `VOID20[C0,unknown_evidence]`. VOID entry: P=1+0, census complete, gap open; reader open; agree.
3. EXIT_D18@L: suspended; arrival valid; no additional discovery fault against the refuted claim (D40); payload disposed.
4. S(25): suspended; no FRONTIER.

Combined: `OPEN0, VOID20[C0,unknown_evidence]`. x=none, P=1+0.

## R5a — (h=1, p=empty, q=empty): `TERM_D18, EXIT_D18@L, S(19), D20@P0`

Initial P=2+0 (D and H unresolved), census complete.
1. TERM_D18 recorded. 2. EXIT_D18 staged.
3. S(19): at L: `EXIT_D18 → F18[C0]`; attempt completes → x=18 commits (D resolved), P=1+0. H unresolved → no closure, no END.
4. D20@P0: attributable TRANS after claimed terminal time refutes the match and contradicts the committed discharge (D40, D43) → FAULT(unknown_evidence,20,C0); revoke x=18, persist `rOPEN0` (D43 "persists a revoked startup OPEN"), invalidate dependent cached answers, D pending (P=2+0); census not reset. Suspend, latch on, D20 discarded, `VOID20[C0,unknown_evidence]`. VOID entry: suspended, latch on, x revoked, P=2+0, census complete, gap open; persisted OPEN0, EXIT_D18, F18, rOPEN0 → open; agree.

Combined: `OPEN0, EXIT_D18, F18[C0], rOPEN0, VOID20[C0,unknown_evidence]`.

## R5b — (h=1, p=AB_D, q=K): `AB_D, TERM_D18, EXIT_D18@L, S(19), K, D20@P0`

1. AB_D accepted (D45); P=2+0. 2. TERM_D18 recorded. 3. EXIT_D18 staged (abandoned lifetime's EXIT is released normally, D45).
4. S(19): `EXIT_D18 → F18[C0]`; x=18 commits; P=1+0; no END (H pending).
5. K: `VOID20[Cr,counter_change]`, latch on, suspended; census complete. VOID entry: x=18 still committed, P=1+0, gap open; reader open; agree.
6. D20@P0 (detection 20, Cr): "Check incoming evidence against committed proofs before dropping its payload during suspension" (D43); 20 > 18 refutes the match and contradicts committed x → FAULT(unknown_evidence,20,Cr); revoke x, `rOPEN0`, D pending P=2+0; then `VOID20[Cr,unknown_evidence]`. D20 disposed (suspension disposal, not abandonment count). VOID entry: suspended, latch on, x revoked, P=2+0, census complete, gap open; persisted includes rOPEN0 → open; agree.

Combined: `OPEN0, EXIT_D18, F18[C0], VOID20[Cr,counter_change], rOPEN0, VOID20[Cr,unknown_evidence]`. Abandoned 0 (D20 = suspension disposal). Uncovered 0.

## R10a — `TRY_NW_D6, ADc6, PTR_D9(non-NULL), CN6, S(10)`

1. TRY_NW_D6: retained observation.
2. ADc6: valid route-(c) ADMIT recorded; adm=true.
3. PTR_D9(non-NULL): refutes the still-unrefuted accepted route-(c) admission (D43) → FAULT(unknown_evidence,20,C0); bound not committed → "this refutation alone publishes no revoked OPEN and does not reset census completeness". Suspend, latch on, `VOID20[C0,unknown_evidence]`. VOID entry: adm=true, proof refuted, P=1+0, census complete, gap open; reader open; agree.
4. CN6: explicitly claims completed continuity despite PTR9 → false claim ("A separately false confirmation still faults", D43; D37(c)) → rejected, FAULT(unknown_evidence,20,C0), `VOID20[C0,unknown_evidence]` (second). No rOPEN.
5. S(10): suspended, no FRONTIER.

Combined: `OPEN0, VOID20[C0,unknown_evidence], VOID20[C0,unknown_evidence]`. unknown_evidence diagnostic 2.

## R10b — continuity-break variant: `TRY_NW_D6, ADc6, BREAK(continuity), CN6, S(10)`

Identical to R10a with the break as the refuting primitive (D43 "demonstrated pointer-observation continuity or ownership break refutes ..."): first `VOID20[C0,unknown_evidence]` at the break, second at CN6 (false completed-continuity claim). No rOPEN, census complete, no FRONTIER.

Combined: `OPEN0, VOID20[C0,unknown_evidence], VOID20[C0,unknown_evidence]`.

## N5 — `NW_D6, S(6), S(8), PTR_D12(non-NULL), S(14)`

1. TRY_NW_D6 (physical history): retained. Wait-word attempt count 0 (NULL read consumes none, D38).
2. ADc6 recorded; adm=true.
3. CN6: valid, matching ADMIT recorded → accepted candidate; b needs completeness strictly beyond 6 (D37(c)).
4. S(6): at L W=6 → `F5[C0]`; completeness only below 6 → no commit.
5. S(8): P0/P1/E: W stays 6. L: W=8 > 6 → b=6 committable; C_close=max(5,6)=6; route (c) triggers no increment → P_close=6. `END(6) → F7[C0]`; attempt succeeds → b=6 and closure commit. P=0+0. D (PG13 logger) not listed or counted; contributes nothing to any population (D43).
6. PTR_D12(non-NULL): refutes accepted route-(c) claim with committed bound → FAULT(unknown_evidence,20,C0); revoke b, `rOPEN0`, invalidate cached answers relying on END(6), D pending (P=1+0), census completeness must be re-established (now not established/unknown) (D43). Suspend, latch on, `VOID20[C0,unknown_evidence]`. VOID entry: b revoked, P=1+0, census unknown, gap open; persisted OPEN0, F5, END(6), F7, rOPEN0 → open; agree.
7. S(14): suspended; no FRONTIER.

Combined: `OPEN0, F5[C0], END(6), F7[C0], rOPEN0, VOID20[C0,unknown_evidence]`.

## N14 — `NW_D6, S(8)`, writer rejects END(6) at 20 with Cw

1-3 as N5 (TRY retained, ADc6 recorded, CN6 accepted).
4. S(8): P0/P1/E: W=0. L: W=8 → attempt: C_close=6, END(6) attempted first (D41 order) → writer rejects (not a publication); counter Cw (writer_push_fail=1). FAULT writer_push_fail: suspend, latch on; no END published → no rollback OPEN needed; no FRONTIER ("No frontier certifies a partially failed release"); b and closure uncommitted ("Publication failure commits neither the affected lower bound nor gap closure", D41). `VOID20[Cw,writer_push_fail]`; capture paused. VOID entry: suspended, latch on, adm=true, b=none, P=1+0, census complete (writer_push_fail does not invalidate census, D31), gap open; persisted OPEN0 → open; agree.

Combined: `OPEN0, VOID20[Cw,writer_push_fail]` (rejected call: END(6)). Storage writer_push_fail 0→1.

## O1 — `B_D6{e=6,α2}, D20@P0{final}, D14@P0{earlier}`

1. B_D6: quiet candidate recorded, proposes t=6.
2. D20@P0{final} (o1): staged; P0 last DATA ts 20, w=20; W=0.
3. D14@P0{earlier} (o2): 14 < 20 → §3 `clock_regression`; also earlier-attempt interval ending 14 > proposed 6 contradicts the candidate (D42). D43 two-fault rule: retain evidence, reject D14 (never staged), then first `VOID20[C0,clock_regression]` (latch set before it), then `VOID20[C0,unknown_evidence]` refuting the candidate. Each increments only its own diagnostic (clock_regression 1, unknown_evidence 1); no storage change; D20 discarded. No frontier published → no epoch invalidation. Latch after each VOID: on.

Combined: `OPEN0, VOID20[C0,clock_regression], VOID20[C0,unknown_evidence]`.

## V4 — `AB_D, TERM_D18, EXIT_D18@L, S(19)` (F18 rejected, Cw); `REARM(21,Cw,...), TERM_D18, S(20)`

1. AB_D accepted. 2. TERM_D18 recorded. 3. EXIT_D18 staged.
4. S(19) at L: `EXIT_D18 → END(18)` succeed; F18[C0] rejected at 20, Cw. FAULT writer_push_fail. END never committed → before VOID persist `rbOPEN0` (plain, not revoked); discard provisional closure; invalidate cached answers relying on END(18); census unchanged. x not committed (D40 clarification "an EXIT output in an attempt that is then VOIDed discharges nothing in that or any later epoch"). Latch on; `VOID20[Cw,writer_push_fail]`.
   - VOID entry: e0 suspended, latch on, x/closure uncommitted, P=1+0, census complete, gap open. Persisted: OPEN0, EXIT_D18, END(18), rbOPEN0 → open. The prefix saved at callback entry evaluated alone gives gap open from 0 (later plain OPEN governs, D41); agrees with public view.
5. REARM(21,Cw): accepted; e1 active, baseline Cw, detection 22.
6. TERM_D18: identical duplicate, idempotent, no fault.
7. S(20): at L W=20 → `F19[Cw]` (no frontier had been published). No discharge, no END (latch + clarification).

Combined: `OPEN0, EXIT_D18, END(18), rbOPEN0, VOID20[Cw,writer_push_fail], F19[Cw]` (rejected call: F18). Merge-only: `EXIT_D18, VOID20[Cw,writer_push_fail], F19[Cw]`. Abandoned 0, uncovered 0.

## A18 — `D9(Y→X)@P0, FAULT(allocation_fail,20,C0), AB_D, S(16)`

1. D9 staged (P0 w=9).
2. FAULT: suspend, latch on, D9 discarded, `VOID20[C0,allocation_fail]`; capture paused. VOID entry: P=1+0, census complete, gap open; reader open; agree.
3. AB_D: accepted in suspension; D9 already disposed → not counted.
4. S(16): suspended, no FRONTIER.

Combined: `OPEN0, VOID20[C0,allocation_fail]`. Final `discovery_abandoned_records`=0 (D9 = fault disposal).

## U15 — `PF, X_24, A26@P0, S(28), TERMU_U24, S(30)`, no REARM

Initial: inventory empty, one numeric observation (PID 50) → P=0+1, census incomplete.
1. AOP (permanent loss, attempts stopped, witnesses consumed): recorded fact.
2. FAULT(unknown_evidence,20,C0): `VOID20[C0,unknown_evidence]`, latch on, suspended. VOID entry: P=0+1, census incomplete, gap open; reader open; agree.
3. X_24 (L o1): arrival validation; disposed in suspension — not uncovered consumption (D40).
4. A26@P0: disposed. 5. S(28): no FRONTIER.
6. TERMU_U24: all acceptance/finalisation requirements met (D40); accepted after the latch; unresolved disposition recorded; the startup gap may not close for the rest of the capture. No consumption (EXIT already disposed). P=0+1 unchanged.
7. S(30): no output.

Combined: `OPEN0, VOID20[C0,unknown_evidence]`. Uncovered 0, abandoned 0.

---

## UNDECIDED

- U1 (H3a, H3b, H3c, H3d): whether D36's "Reject an ADMIT contradicted on delivery without recording" applies to a route-(c) ADMIT contradicted by retained D12. I applied it (adm=false). Under the other reading (record, then refute): H3a's TRY would fault a second time (D38); H3d's AB_D would be barred and fault. H3b/H3c come out the same either way.
- U2 (H7a): the contract says the earlier-attempt record contradicts the prospective bound (D42). It does not say whether the recorded ADMIT's proof becomes refuted or stays valid. Neither reading changes any publication.
- U3 (G4 v1): whether the retained TERM_D18 claim is marked refuted after the time conflict revokes its discharge, or stays unrefuted with its discharge revoked. Neither reading changes any publication, because the latch blocks re-commitment.
- U4 (G6): no sentence directly covers a numerical ADMIT delivered after a retained TRY_NW observation. My answer (FAULT, ADMIT rejected) is inferred from D39, D42 and D43. The other reading is that the ADMIT is recorded with no fault, which gives no VOID and the frontier `F18[C0]` at S(19). D's numerical records would still be withheld.
