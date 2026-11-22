# Discovery enumeration triage: product 003ee35a (round 12) + enum test files f477ec0b, 2026-11-21 (written 22:17 UTC)

Paths: `M` = `contracts/merge-v1/CONTRACT.md`; `R` = `oracle-discovery/ROWS.md`; `E` = `oracle-discovery/EXPECTED.md`;
`mc` = `src/core/merge.c`; `host` / `model` = `tests/discovery_enum_{host.c,model.h}`; `dah` = `tests/discovery_admission_host.c` (the product's own oracle tier).
All line numbers refer to the unmodified files. Those files are identical in `wt-m12r` and in `W/base` (`diff -r` printed nothing).
Work dir `W` = `a2go/disc/m12/triage-work/` under the session scratchpad. It holds two scratch worktrees of 003ee35a with the six test files checked out: `base` (unchanged) and `fix` (instrumentation and switches).
I edited nothing in `wt-m12r` or in any other existing worktree. I committed nothing. Logs are in `W/runs/`, and the switch diff is `W/runs/switches.patch`.

## Method (VERIFIED)
- `W/fix` instrumentation was ported from round 11. `DE_DUMP` prints every failing (scope, class, descriptor) to stderr, and I deduplicated the output with `sort -u`. `DE_MTRACE` prints model `M[...]` and product `P[...]` timelines per compared prefix (`W/mt.sh BUILD FAMILY WORD FAULT INV`).
- Baseline: I ran the instrumented binary (before any switch was added) over the full `--only all --depth 4`, producing `W/runs/d_base.log` and `W/runs/d_base.u.tsv`.
  It reproduces `enum-run/full.log` exactly: DATA 1,551 / FRONTIER 1,206 / VOID 1,992 / report 1,257 / fault_diagnostics 1,992 / publication_order 1,992, with the same minimal words.
  It has 0 pin failures and 0 invariant failures.
- Two switches, each env-gated in `W/fix`:
  - `P_REJ` is on the product side (reading A): mc:818 drops the `enable_exit_refuted &&` gate.
  - `M_REC` is on the model side (reading B): model:369, before the rejection branch, records a control that is contradicted on delivery as the refuted final claim when no Decision 36 contradiction exists.

## Cause G2: a terminal control contradicted on delivery by the retained EXIT, followed by the matching control. CONTRACT GAP (both sides VERIFIED)

**Counts (VERIFIED, `W/runs/d_base.u.tsv`).**
- There are 1,992 distinct failing descriptors, and every one contains `EXIT18 < TERM19_CONFLICT < TERM18` as a subsequence.
- Every descriptor with that subsequence fails, except one case: FINAL_TRANS20 precedes TERM18. Then TERM18 is itself separately false (TRANS20 > 18) and both sides fault.
- Breakdown:
  - C family: (1 three-token word x 5 fault gaps + 52 four-token words x 6) = 317 per inventory.
  - N family: 365 - 18 = 347 per inventory.
  - Total: 3 x (317 + 347) = 1,992. L has no TERM19_CONFLICT.
- Class counts are subsets of the same 1,992: VOID, fault_diagnostics and publication_order 1,992 each; DATA 1,551; report 1,257; FRONTIER 1,206.
- Counting method: every baseline pair is cleared by either switch alone. The switched dump files are empty (0 lines), and no new pair appears.

**Shortest schedule `C:H0:[EXIT18,TERM19_CONFLICT,TERM18]:fault=-1` (VERIFIED, DE_MTRACE, `W/runs/orders_fm1.txt`)**
| prefix | input | model | product |
|---|---|---|---|
| 2 | EXIT18 (D, cookie, L#1) receipt | `O0` | `O0` |
| 3 | TERM19_CONFLICT (names L#1, x=19) | `O0 V7@20` | `O0 V7@20` |
| 4 | TERM18 (names L#1, x=18) | `O0 V7@20` | `O0 V7@20 V7@20` |
| 5..12 | S40, S41 | unchanged | unchanged |

**The same word with injected faults (VERIFIED, `W/runs/w1_faults.txt`).**
- fault=2: model `O0 V7@20e1 V10@20e1 ... F39e2 F40e2`; product `O0 V7@20e1 V10@20e1 V7@22e2 F39e3 F40e3`. The extra VOID means an extra REARM epoch, which accounts for the FRONTIER/report/DATA (stream order) classes.
- fault=3: model `... V7@20 V10@20 F39e2`; product `... V7@20 V7@20 V10@20 F39e2`. Fault 0 and fault 1 give the same extra `V7@22`.

**The other five orders of the three tokens all agree, each with 0 failures (VERIFIED).**
- EXIT18,TERM18,TERM19: 1 fault, at TERM19, which conflicts with the retained claim 18.
- TERM19,EXIT18,TERM18: 2 faults. EXIT18 refutes the recorded claim 19. TERM18 conflicts with that retained, refuted claim (M:460 "including when the retained claim is refuted").
- TERM19,TERM18,EXIT18: 1 fault.
- TERM18,EXIT18,TERM19: 1 fault.
- TERM18,TERM19,EXIT18: 1 fault.

The difference exists only when TERM19 is contradicted on delivery by an already retained EXIT and a qualified TERM18 follows it.

**Product (VERIFIED by reading and by the P_REJ switch).**
- At TERM19, mc:818-824 is the "reject a separately false control before recording" path. It runs only when `a->discovery.enable_exit_refuted`, and D has no ADMIT here, so it is skipped.
- mc:825-831 then records `terminal_claimed`, `claimed_terminal_time=19`. mc:864-867 calls `discovery_refute_terminal`, which faults because `exit_stamp 18 != 19` (mc:580-585). TERM19 is now the retained, refuted final claim.
- At TERM18, mc:832-836 sees `claimed_terminal_time 19 != 18`, sets refuted and faults.
- The gate at mc:818 was introduced in 003ee35a itself, for H11e (`git show 003ee35a`: "reject separately false terminal controls before retaining their final claims").

**Model (VERIFIED by reading and by the M_REC switch).**
- At TERM19, model:366-368 sets `separately_false`, because `exit_seen && exit_t != o.t`. model:369-374 rejects the control into `rejected_term_*` without setting `l->terminal`, and faults.
- At TERM18, the duplicate check (model:361-363) does not match, `separately_false` is false and `l->terminal` is false. model:375 records the claim, with no fault.
- The model applies this rejection whether or not a Decision 36 contradiction exists. The test author says so in evidence rule 7: "The same rejection rule applies to other terminal controls already contradicted on delivery".

**Reading A (model: rejected on delivery, so the later match is not a conflict).**
- M:360: "Reject a separately false terminal control before recording a final terminal claim or applying this same-contradiction suppression; preserve its ordinary fault and required refutation history, so a later otherwise qualified match is not rejected merely for disagreeing with that rejected control."
- M:460: "Reconciling that EXIT ... against an already refuted terminal claim produces no additional discovery fault ..., including when the claim was rejected on delivery and only its refutation history was retained." This means a claim can be rejected on delivery and leave only refutation history.
- M:460: the conflict fault applies to a control that "conflicts ... with a **retained final terminal claim**". A rejected control is not one.
- Rows:
  - E:197 (H6b): "TERM_D19, which retained D20 already contradicts; no accepted terminal disposition or discharge results". E:420 mutant: "Record a terminal claim already contradicted on delivery as an accepted proof".
  - E:449 (H11e mutant): "Retain rejected TERM_D17 as the final claim and fault the later otherwise qualified TERM_D18 for conflicting with it". This is exactly the product's behaviour, but pinned only with AD_D19 present.
  - Both delta-8 readers took U1 reading (i), "rejected" (answers-delta8-opus.md:325; answers-delta8-sol.md H6b table; compare-delta8.md:63).

**Reading B (product: outside Decision 36, the contradicted control is the retained refuted claim, and later disagreeing controls conflict with it).**
- The M:360 sentence sits inside the Decision 36 enable/termination paragraph. "Separately false" is defined there relative to that contradiction, so its "later qualified match" clause need not extend to lifetimes without an ADMIT.
- M:460: "A terminal claim is refuted once ... Retain enough refutation history to enforce this rule and prevent restoration of the claim". Also M:460: "conflicts in ... terminal time or named EXIT transport identity with a retained final terminal claim reports FAULT ..., including when the retained claim is refuted and no discharge ever committed."
- Rows: E:198 (H6c) "The separately delivered conflicting TERM_D21 faults independently; it neither replaces nor restores the first claim". E:421 mutant: "Suppress a separately delivered conflicting terminal control because an earlier claim was refuted".
- The product pins this reading in its own oracle tier. dah:3480-3484 `G_r11_rejected_terminal_then_conflict` runs `D20, TERM19, REARM, TERM21` and expects `VOID20 VOID22` ("conflicting terminal control faults after a rejected first claim") while also asserting `disposition_count==0`. That is the H6b-order variant of H6c, with no contract row.

**Why it is undecided (VERIFIED by reading).**
- No row has a qualified match after a control that was contradicted on delivery, with no ADMIT. H6b has no later TERM. H6c's first claim was recorded before its contradiction. H11e has the AD_D19 enable contradiction. G4 and G4b conflict with an accepted, discharged claim.
- Delta 8 listed exactly this as UNRESOLVED U1 (answers-delta8-opus.md:325: "rejected ... or recorded as final MATCHED disposition with claim refuted"). It then declared "Outputs, faults and counts are identical under both readings" (compare-delta8.md:63, "No. Stream, fault total 1 ... identical").
- That declaration is false. A later qualified TERM makes the fault count, the VOID stream and the epoch count differ, as shown in the traces above.
- SEARCHED: R and E for rows with TERM after a contradicted-on-delivery TERM (G4, G4b, H6a-c, H10a/b, H11a-f read in full); compare-delta8.md in full; the answers-delta8-opus.md UNRESOLVED section; `grep 'r11_rejected_terminal\|rejected first claim'` over `contracts/` and `docs/`, which found nothing.
- NOT SEARCHED: the full text of answers-delta7/delta6 or the earlier comparisons, and any decision doc outside `contracts/merge-v1`.

**Lean (INFERRED).** The text and both independent readers favour A. "Retained final terminal claim" and "rejected on delivery and only its refutation history was retained" do not fit B. However, the only row that pins A (H11e) carries the Decision 36 premise, and the product's own test pins B. Someone has to decide whether to extend the H11e rule to lifetimes without a Decision 36 contradiction. Candidate rows: `EXIT_D18@L, TERM_D19{names that EXIT}, TERM_D18` and `D20@P0, TERM_D19, RA, TERM_D21`.

## Class x cause (failing descriptors, baseline = full.log)
| class (scope) | total | G2 |
|---|---|---|
| model.VOID (enum) | 1,992 | 1,992 |
| model.fault_diagnostics (enum) | 1,992 | 1,992 |
| model.publication_order (enum) | 1,992 | 1,992 |
| model.DATA (enum) | 1,551 | 1,551 |
| model.report (enum) | 1,257 | 1,257 |
| model.FRONTIER (enum) | 1,206 | 1,206 |
| pins / invariants | 0 | — |

No PRODUCT DEFECT and no TEST DEFECT was found. There is one cause, a contract gap.

## After-switch runs (VERIFIED)
| run | positive | pins | enumeration | dump lines | log |
|---|---|---|---|---|---|
| baseline | 0 classes | 0 | 6 classes (above) | 1,992 descriptors | `W/runs/d_base.log` |
| `P_REJ=1` (product to reading A) | 0, totals [18,9,9,12,6,12,4,12] | 0 | **0 classes**, exit 0 | 0 | `W/runs/d_P_REJ.log` |
| `M_REC=1` (model to reading B) | 0, same totals | 0 | **0 classes**, exit 0 | 0 | `W/runs/d_M_REC.log` |

- Both switched runs have no enumeration non-vacuity counter at 0. `--self-check` stays fully caught (`all_fake_branches_caught:true`, `deleted_model_rule_caught:true`) under none, P_REJ and M_REC.
- Product oracle tier (`tests/test_discovery_admission.py`):
  - Plain: exit 0, "183 oracle rows, 226 named cases passed" (`W/runs/oracle_plain.log`).
  - With P_REJ: exit 1, at `ASSERT G_r11_rejected_terminal_then_conflict ... (line 3483)` (`W/runs/oracle_PREJ.log`).
  - Running each of the 409 product cases separately under P_REJ: exactly that 1 fails (`W/build-oracle/prej_cases.txt`). H6b, H6c, H11e, G4 and G4b PASS.
  - So reading A breaks only the product's own non-contract case. Reading B breaks nothing in either suite.

## What I could not determine
- Which reading is right. This is an owner or adviser decision. I record a lean, not a verdict.
- Whether the product's `disposition_count==0` for the rejected TERM19 alongside the internal `terminal_claimed=true` (mc:826) would be visible as a contradiction through a public EXIT-disposition accessor. None is exposed (evidence.txt:30). INFERRED from reading only.
- The TRANS variant (dah:3481, a TERM contradicted by a retained D20) is not reachable in the enumeration. C's TRANS12 is below 18, and N's FINAL_TRANS20 also makes the following TERM18 separately false. So the enumeration exercises only the EXIT-timestamp variant.
- Not run: mutants (`--mutants`) of the product oracle tier, sanitizer builds and any box tier.
- Scratch worktrees `W/base` and `W/fix` are registered in the main repo's worktree list. `W/fix` has uncommitted switch edits in `src/core/merge.c` and the model. Remove both with `git worktree remove --force <path>` when done.
