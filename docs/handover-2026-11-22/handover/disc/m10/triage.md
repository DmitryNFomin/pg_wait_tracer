# Discovery enumeration triage, product b5ae3fb6 + enum test files (wt-m10), 2026-11-21

Paths: `M` = wt-m10 `contracts/merge-v1/CONTRACT.md`; `R` = `oracle-discovery/ROWS.md`; `E` = `oracle-discovery/EXPECTED.md`;
`mc` = `src/core/merge.c`; `cap` = `src/merge_capture.c`; `host` / `model` = `tests/discovery_enum_{host.c,model.h}` (all in wt-m10).
Work dir `W` = `a2go/disc/m10/triage-work/` (under the session scratchpad). I edited nothing in any git worktree. The product patches below exist only in the copy `W/fix/ptree/`.

## Method (VERIFIED)
- `W/dump/` is a copy of the host with one dump line in `de_fail` (class, descriptor, prefix, op, model/product ADMIT/ABANDON, `admission_recorded`, latches,
  VOID counts, in-callback flag) and an optional per-prefix model-vs-product trace (`W/mt.sh FAMILY WORD FAULT INV`). The full run reproduces `full.log` exactly:
  the same 10 failure rows and counts (`W/dump/full.stdout`; 188,337 failing (class, descriptor) pairs in `W/dump/fails3.raw`).
- `W/fix/` turns eight candidate causes into switches (`DE_FIX=...`). Three are test switches (T1, T2, plus G1-G4 in the model), and two are product switches (P1, P2)
  in the product copy `W/fix/ptree`. Runs: baseline, each switch alone, all switches, and all-but-one for each switch (`W/fix/runs/*.out|err`).
  **With all eight switches, every class is 0 in all three scopes** (enumeration, pins, positive). The positive cohort stays [18,9,9,12,6,12,4,12].
- Counting: for each baseline failing (class, descriptor), "necessary" means it still fails when only that switch is off. "Sufficient" means that switch alone clears it.
  Every pair has a non-empty necessary set. The partition below adds up exactly to each class total.

## Causes

**T1. The driver reads `admitted` as "recorded ADMIT"; the product records an ADMIT made under the latch in `admission_recorded`. TEST DEFECT (INFERRED)**
- Counts: model.recorded_controls 157,083 alone, plus 7,380 that also need P1 (total 164,463). Pins: recorded_controls 2 and pin.complete_row 2.
  The pins are **G1 and G2/prefixed**, one each, not two from G1 (VERIFIED from the dump).
- Shortest word: `C:H0:[ADc6]:fault=0` (FAULT, REARM, ADc6). After ADc6 the product shows `admission_recorded=1, admitted=0`; the model shows admitted=1 (VERIFIED trace).
- Product: mc:839 sets `admission_recorded`. mc:845-846 sets `admitted = status==OK && !latched`, the certification-eligible flag.
  The product honours the recording later: a late TRY after a latched ADMIT faults (mc:712-715), which is why the G2/prefixed stream passes.
- Test: host:300 maps `v->admitted = s.admitted`. model:249-254 sets admitted with no latch condition. The pin expectations at host:1321/1325 pin public `admitted=true`.
- Contract: M:504 "A valid subsequent control is recorded without a fault caused by the latch ... Recorded controls do not authorize recovery-time admission or restore discovery certification".
  M:426 "a valid ADMIT recorded under the Decision 43 latch". E:160-161 (G1, G2/prefixed): "ADc6 is subsequently recorded".
  The contract requires recording, and the product's interface exposes recording as `admission_recorded` (pgwt_discovery.h:155-158). The driver reads a different field.
- Caveat (INFERRED): neither field is documented in the header. If the owner wants `admitted` to mean "recorded", this becomes a product change at mc:845.

**P1. ABANDON is accepted after an ADMIT recorded under the latch. PRODUCT DEFECT (INFERRED from text; no row)**
- Counts: model.post_fault_control_VOID 7,356 alone, plus 9 with G1, plus 15 with G2 (7,380). The same 7,380 also differ in recorded_controls (T1+P1).
- Shortest word: `N:H0:[AD6,AB]:fault=0`. The contract requires FAULT(unknown_evidence) at AB.
  The product accepts AB with no fault (`abandoned=1` beside `admission_recorded=1`). The model faults with V7@22 (VERIFIED trace).
- Code: mc:748-752 refuses ABANDON only when `a->discovery.admitted`. That flag is false for every ADMIT recorded under the latch, so it is false for every ADMIT in a post-REARM epoch.
- Contract: M:533 "Accept a first abandonment only for ... identity with no accepted ADMIT in any epoch of the capture ... An invalid abandonment reports FAULT(unknown_evidence,d,C)".
  M:535 "An already admitted identity cannot be abandoned". M:504 and M:426 (above) say such an ADMIT is recorded.
  "In any epoch" would be vacuous for later epochs if a latched ADMIT did not count. Fix: test `admission_recorded` at mc:751. With P1 alone, this class drops 23,539 → 16,183 (VERIFIED).

**P2. A route-(c) ADMIT is recorded before it faults on earlier numerical DATA. PRODUCT DEFECT (INFERRED; nearest row G6)**
- Counts: post_fault_control_VOID 2,385. Under T1 these show up as 14,037 additional recorded_controls differences. Those are absent at baseline and induced by the T1 read.
- Shortest words: `C:H0:[TRANS12,ADc6,TRY_NW6]:fault=-1`, with the same pattern for `PTR_NONNULL` and `PTR_BREAK` in place of TRY.
  Both sides fault at ADc6 (V7). Afterwards the product faults again at TRY (mc:712-715, treated as a late TRY) or at PTR (mc:721-724, because `no_wait_word` is set). The model does not (VERIFIED trace).
- Code: mc:837-842 sets `no_wait_word`, `admission_recorded` and the baseline. Only then does mc:843-844 fault on `numerical_seen`.
  By contrast, the numerical-route contradiction at mc:766-768 faults before recording anything.
- Contract: M:354 "Contradictory provenance ... reports FAULT(unknown_evidence)". M:506 "An attributable TRANS/MARKER contradicts a permanent no-wait-word proof".
  M:426 and M:504 record *valid* ADMITs. E:167 (G6, a contradicted numerical ADMIT): "no accepted admission".
  Fix: test `numerical_seen` before mc:837. With P2 alone, this class drops to 21,154 (VERIFIED).

**G1. Controls after a late TRY that followed a numerical ADMIT. CONTRACT GAP**
- Counts: post_fault 903 alone, plus 3 with G2, plus 9 with P1.
- Shortest word: `N:H0:[AD6,TRY_NW6,B6]:fault=-1`. Both sides fault at TRY. At B6 the model faults again; the product does not (VERIFIED trace).
- Product: mc:712-715 faults and returns before mc:717, so the TRY's NULL-pointer history is not retained.
- Model: model:240-243 sets `address_refuted`. That flag makes any later B or AD fault (model:252, model:257).
- Contract: M:426 says the late TRY "is not recorded, whatever history it names" and is "also evidence against the admission's address proof and revokes any committed numerical lower bound".
  Reading A (product): it only revokes the bound committed at that moment, and later controls are judged without it.
  Reading B (model): the address proof stays refuted, so later numerical claims are contradictory. No row: G2b and G3/revocation end at the TRY.

**G2. A quiet candidate recorded before a fault, then contradicted after REARM. CONTRACT GAP (product rule is order-dependent)**
- Counts: post_fault 8,884 alone, plus 3 with G1, plus 15 with P1.
- Shortest word: `N:H0:[B6,EARLIER_TRANS14]:fault=1`. After REARM the model faults on D14 (14 > quiet t=6); the product does not (VERIFIED).
  Control: with the fault before B6 (`fault=0`) the product does fault on D14 (VERIFIED). A CONFIRM made after the fault is retained; one made before is dropped.
- Product: `fault()` at mc:259-263 clears `quiet_pending` on every VOID. Model: `de_model_fault` (model:67-74) keeps `quiet`, and model:318 faults on candidate_bad.
- Contract: M:490 "Reconcile a proposed or committed numerical lower bound ... reports FAULT". M:504 "Preserve previously committed, unrefuted proofs ..." does not list candidates.
  M:506 covers committed proofs only. M:508 says "proposed or committed", but only for a two-fault observation.
  Reading A (product): a VOID disposes of uncommittable candidates. Reading B (model): a recorded CONFIRM stays a retained candidate across VOID and REARM, like a post-fault CONFIRM.

**G3. A second contradiction of a terminal claim that evidence has already refuted. CONTRACT GAP**
- Counts: post_fault 3,984.
- Shortest word: `N:H0:[FINAL_TRANS20,TERM19_CONFLICT,EXIT18]:fault=-1`. TERM19 is refuted by D20 and both sides fault.
  At EXIT18 (≠19) the model faults again; the product does not (VERIFIED trace).
- Product: `discovery_refute_terminal` returns false once the claim is refuted (mc:565).
  A conflicting second TERM control faults at mc:789-791 without marking the claim refuted, so a later EXIT does fault again. The switch models exactly this; an earlier, broader switch failed.
- Model: model:321-333 faults on every contradicting EXIT or TRANS.
- Contract: M:460 "Report FAULT(unknown_evidence,d,C) when the contradiction is established ... An identical retransmission ... repeats no already established contradiction report".
  M:508 says one unknown_evidence "for all discovery contradictions that observation establishes". R2 (E:130) only has an EXIT that agrees with the claim.
  Reading A: one fault per refuted claim. Reading B: one fault per contradicting observation.

**G4. Contradiction inside a batch that a missing TEXT dependency holds. CONTRACT GAP**
- Counts: model.latch, model.VOID, model.report and model.fault_diagnostics, 12 each. They are the same 12 descriptors: the four positions of AD6 times H0/H1/H2.
- Shortest word: `N:H0:[AD6,FINAL_TRANS12,EARLIER_TRANS14,HOLD_A20]:fault=-1`. At S40 the eligible batch is {D12, D14, A20 (requires x)}.
  The product faults V7@20 while validating D14 against the prospective b=12 (mc:1068-1074, then mc:997-998), before its held return at mc:1109-1115.
  The model returns "held" on A20 before any reconciliation (model:142-147). Both VERIFIED (trace plus model debug print `W/dbg/`).
- Contract: M:109-110 "Before release, validate the complete eligible batch, identities, dependencies ... A dependency not yet available holds the batch" supports the product.
  M:364 (the bound comes from the first *successfully released* record) and M:480 ("never prospective bounds ... left by a held ... release") support the model.
  M:490 says "proposed" without defining it for a held batch. G8 (E:169) holds a batch with no contradiction in it, so it does not decide.

**T2. The closed-view coherence invariant is sampled inside the VOID callback. TEST DEFECT (INFERRED)**
- Counts: views.closed_pending_census 270 enumeration plus 13 pins (G2b, G3/revocation, G4/time, G4/identity, G4b, 44612ec2, R3 x2, R4 x2, R5 x2, R12).
  All 283 have in-callback=1 (VERIFIED), and every one shows the callback state `closed=1, pending=1+0` (VERIFIED for all 270).
- Shortest word: `C:H0:[ADc6,CN6,S19,PTR_NONNULL]:fault=-1`. After the primitive completes both sides agree: closed=0, revoked OPEN (VERIFIED).
- Product: `discovery_revoke` clears the bound (mc:551-561), and then `fault()` publishes VOID through the sink (mc:276).
  `startup_closed` is cleared only after `pgwt_merge_feed` returns (cap:247, cap:263-265). Status computes `closed` from `startup_closed` but computes pending live (cap:636-655).
- Test: host:471-499 re-observes inside every sink callback, and host:440-442 checks coherence there. host:493-495 states this deliberately.
- Contract: R:56 and R:63 "observe ... after every primitive input". Neither the contract nor the header defines a status read inside a callback.
  No in-tree consumer reads discovery status from a sink (grep of `pgwt_capture_discovery_status` in src: cap:680, 829 and 890, numeric counts only).
- Alternative: if the owner wants status atomic with VOID publication (RULE 2, "two views agree"), the product should clear `startup_closed` before the core publishes VOID.

## Class x cause (failing descriptors; "+" means both switches are needed)
| class (scope) | total | T1 | T1+P1 | P1 | P1+G1 | P1+G2 | P2 | G1 | G1+G2 | G2 | G3 | G4 | T2 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| recorded_controls (enum) | 164,463 | 157,083 | 7,380 | | | | | | | | | | |
| post_fault_control_VOID (enum) | 23,539 | | | 7,356 | 9 | 15 | 2,385 | 903 | 3 | 8,884 | 3,984 | | |
| latch / VOID / report / fault_diagnostics (enum) | 12 each | | | | | | | | | | | 12 | |
| closed_pending_census (enum) | 270 | | | | | | | | | | | | 270 |
| closed_pending_census (pins) | 13 | | | | | | | | | | | | 13 |
| recorded_controls / pin.complete_row (pins) | 2 / 2 | 2 / 2 | | | | | | | | | | | |

## Not determined
- Every verdict above is a reading of the contract; no canonical row pins any of the eight situations. SEARCHED: R and E for latch, abandon-after-admit, held-batch, refuted and VOID-vs-candidate rows (G1-G8, R1-R12, U rows).
  NOT SEARCHED: answers-*.md, compare-*.md.
- Whether `admitted` (T1) is meant as a public "recorded" flag is not stated anywhere, so T1 against a product change at mc:845 is a field-semantics choice.
- I did not run the product's own oracle assertions (`tests/discovery_oracle_assertions.txt`) or any box tier. The enumeration's non-vacuity counters were not re-inspected under the fix switches.
