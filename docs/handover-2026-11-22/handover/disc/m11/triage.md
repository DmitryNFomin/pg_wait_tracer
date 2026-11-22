# Discovery enumeration triage, product f3ee42e3 (round 11) + enum test files 0ef37864, 2026-11-21

Paths: `M` = `contracts/merge-v1/CONTRACT.md`; `R` = `oracle-discovery/ROWS.md`; `E` = `oracle-discovery/EXPECTED.md`;
`mc` = `src/core/merge.c`; `cap` = `src/merge_capture.c`; `hdr` = `src/core/pgwt_discovery.h`;
`host` / `model` / `rows` = `tests/discovery_enum_{host.c,model.h,rows.h}`. All line numbers refer to the unmodified files in `wt-m11r`.
Work dir `W` = `a2go/disc/m11/triage-work/` under the session scratchpad. I created three scratch worktrees of f3ee42e3 there with the six test files checked out: `base` (unchanged), `fix1` (step 1) and `fix2` (env switches).
I edited no product file and nothing in `wt-m11r`. I committed nothing. Run logs are in `W/runs/`.

## Method (VERIFIED)
- `W/base` reproduces `enum-run/full.log` exactly: the same 17 failure rows and counts (`W/runs/base.log`).
- `W/fix2` is the driver/model copy with switches. `T_NOCONT` turns the step-1 input off. `T_H` is the driver REARM fix (cause T2). `G_TERM` is the model reading A (cause G1). `T_END` is the invariant fix (cause T3). `DE_DUMP` prints one line per (scope, class, descriptor).
  `DE_MTRACE` prints the model and product ordered timelines per compared prefix, using `W/mt.sh FAMILY WORD FAULT INV`.
- Counting: I dumped every failing enumeration (class, descriptor) pair in four runs: {continuity off/on} x {T_H off/on}. That gives 390,736 baseline pairs (`W/runs/d_*.tsv`). Each baseline pair is assigned by which switch clears it.
  No switch combination introduced a new pair (0 pairs outside the baseline set in any run). The cause columns below add up exactly to every class total.
- With continuity + T_H + G_TERM + T_END, all three scopes are green and the exit code is 0 (`W/runs/d_allfix.log`). Positive totals stay [18,9,9,12,6,12,4,12], and the depth-4 non-vacuity guard passes.
  `--self-check` with T_END still reports `all_fake_branches_caught:true`, including `latch_END` (VERIFIED).

## Step 1: driver supplies `current_epoch_continuity` (VERIFIED)
Change, in `W/fix1` only (driver and row file; product untouched):
- host:874 (ADMIT encoding for AD/ADc/AD_FALSE/ADc_FALSE): `a.current_epoch_continuity=!op.missing_continuity;`.
  This follows R:221 "A valid startup ADMIT delivered after RA supplies its own independently qualified continuity evidence ... through RA and through the current observation". It also follows M:504 "When that startup ADMIT is delivered after an accepted REARM, its independently qualified proof supplies the current epoch's attachment evidence ...".
  Only H11c's op sets `missing_continuity`. Per R:261 it "supplies no proof of continuity through RA", which matches M:504 "A historical startup ADMIT whose original admission facts are valid is recorded even when evidence extending that attachment ... is missing".
- H11c encoded: rows:126 `model_only=false`, so H11c drives the product with `current_epoch_continuity=false`. The REQUIRED_API failure at rows:170 can no longer fire.

| scope | class | before (full.log) | after step 1 (`W/runs/fix1.log`) |
|---|---|---|---|
| pins | model.DATA (H2a) | 1 | 0 |
| pins | model.FRONTIER (H2a) | 1 | 0 |
| pins | model.VOID (H2a) | 1 | 0 |
| pins | model.report (H2a) | 1 | 0 |
| pins | model.fault_diagnostics (H2a) | 1 | 0 |
| pins | pin.complete_row (H2a) | 1 | 0 |
| pins | model.publication_order (H2a) | 1 | 0 |
| pins | pin.VOID_entry (H2a, "extra VOID callback") | 1 | 0 |
| pins | latch.END (V4) | 1 | 1 |
| pins | driver.H11c_continuity_unrepresentable | 1 | 0 (H11c product pin driven and **passes**: `"pin","name":"H11c","red":false`) |
| enum | model.DATA | 22,052 | 138 (minimal `L:H0:[AD19,TERM18,EXIT18]:fault=3`) |
| enum | model.FRONTIER | 73,444 | 49,662 (minimal `C:H1:[HOLD_A20]:fault=0`) |
| enum | model.VOID | 73,819 | 50,037 |
| enum | model.epoch_invalidation | 54 | 0 |
| enum | model.report | 73,729 | 49,947 |
| enum | model.fault_diagnostics | 73,819 | 50,037 |
| enum | model.publication_order | 73,819 | 50,037 |

## Causes

**T1. The driver never supplies `current_epoch_continuity`. TEST DEFECT (VERIFIED)**
- Counts (baseline): enum VOID/FRONTIER/report/fault_diagnostics/publication_order 23,782 each, DATA 21,914, epoch_invalidation 54. Each is cleared by step 1 alone.
  A further 848 per class (not DATA) need both T1 and T2. Pins: all 8 H2a classes and the H11c REQUIRED_API row.
- Shortest schedule: `C:H0:[ADc6,EXIT18]:fault=0`. The sequence is FAULT, REARM (epoch 2; D not in the REARM proofs because no ADMIT has been recorded yet), ADc6, EXIT18. At S40's last boundary the product publishes `V7@22` and the model publishes `X18U F39` (VERIFIED trace). With continuity supplied, the run has 0 failures (VERIFIED).
- Product: mc:886-889 sets `identity_proved` from `epoch==1 || d->current_epoch_continuity`. The REARM at mc:1554-1564 cleared it for every unsupplied attachment, so mc:1043 faults UNKNOWN at the first eligible batch. This is the M:504 rule "Missing ... continuity reports FAULT(unknown_evidence,d,C) at that lifetime's first eligible-batch validation". Product is correct.
- Test: host:870-876 builds the ADMIT and never sets the new field, which zero-initialises to false. This is the driver written against the pre-round-11 header (hdr:114-116).

**T2. The driver's REARM omits H's continuing attachment in inventory H1. TEST DEFECT (VERIFIED mechanism; INFERRED that R:221 extends from D to H)**
- Counts (after step 1): VOID/report/fault_diagnostics/publication_order 49,878 each and FRONTIER 49,596. In baseline terms that is 49,030 T2-only plus 848 T1+T2 (FRONTIER 48,748 + 848).
  All of them are H1 descriptors (VERIFIED from the dump).
- Shortest schedule: `C:H1:[HOLD_A20]:fault=0`. The H1 prologue records AD_H8 for H (pid 40) in epoch 1 (host:1000-1010). Then FAULT and REARM. HOLD_A20 delivers A20 (needs text) and H21.
  At S40's L boundary the product faults `V7@22`; the model does not (VERIFIED trace). With `T_H`, the run has 0 failures (VERIFIED).
- Test: host:843-844 adds D's proof when `facts.ad||facts.adc` and H's proof only when `inventory==2`. For H1, H's admission was recorded before the fault, but its attachment proof is never supplied to REARM. The model's REARM (model:410-412) does not model attachment proofs at all.
- Contract: R:221 "RA ... fresh valid A/B attachment_proofs, including D's independently qualified continuing attachment when D has a valid recorded ADMIT". M:504 "Delivery of that evidence inside REARM is not required for a lifetime whose startup ADMIT was still pending" implies that it is required for an already recorded ADMIT. Without it, M:504 mandates the product's fault.
- Product: same path as T1 (mc:1554-1564, then mc:1043). Product is correct for the input it was given. The fix is to include pid 40 in the REARM proofs whenever H has a recorded ADMIT.

**G1. A terminal control contradicting a recorded ADMIT, followed by the matching EXIT. CONTRACT GAP (both sides VERIFIED)**
- Counts: 159 descriptors. VOID, fault_diagnostics and publication_order 159 each; DATA 138; report 69; FRONTIER 66. They are exactly every word containing AD19 < TERM18 < EXIT18 as a subsequence:
  3 inventories x (1 three-token word x 5 fault gaps + 8 four-token words x 6 fault gaps) = 159 (VERIFIED from the dump).
- Shortest schedule: `L:H0:[AD19,TERM18,EXIT18]:fault=-1`. Both sides fault `V7@20` at TERM18. At EXIT18 arrival the model faults a second `V7@20`; the product does not (VERIFIED trace).
  With `G_TERM`, the run has 0 failures, and none of the 159 remain (VERIFIED).
- Product: mc:843-850. At TERM, `admitted && baseline.e > terminal_time` sets `admission_refuted = enable_exit_refuted = true`, revokes, and faults. At EXIT arrival, mc:1361-1368 computes `first = !admission_refuted || (terminal_claimed && !terminal_refuted)`, which is false, so there is no second fault.
- Model: model:341-346 at TERM refutes only the terminal claim (`l->refuted`). At EXIT, model:380-385 `enable_bad = admitted && !admission_refuted && enable > t` is true, so it faults again.
- Reading A (product): a TERMINAL control is a kernel terminal proof bound to the named EXIT, so M:360 applies at TERM. M:360: "Successful enable at `e` and independently bound termination at `x<e` ... dispute both the admission provenance and the terminal-time proof, regardless of delivery order ... Refute each recorded claim among them".
  After that, M:504 "Each recorded proof claim is refuted at most once ... later observations do not repeat that contradiction report" and M:460 "Reconciling that EXIT ... against an already refuted terminal claim produces no additional discovery fault" both apply.
- Reading B (model): M:460 "An independently bound lifecycle EXIT is not a newly delivered terminal-control claim". So TERM18 refutes only its own claim, against attachment history (M:460).
  The enable/EXIT contradiction is first established by the EXIT: M:360 "If the ADMIT was validly recorded before the EXIT established the contradiction ... refute its proof. The observation establishing the contradiction reports FAULT". That makes a second, distinct claim refutation.
- No row decides it. H11a (R:259) is AD19, EXIT18, TERM18; H1d is equality. SEARCHED: R and E for AD_D19/TERM-before-EXIT rows; `grep "AD_D19, TERM"` over oracle-discovery/*.md found nothing. NOT SEARCHED: the full text of answers-delta*/compare-delta* beyond that grep.

**T3. V4: invariant `latch.END` counts the faulting attempt's own pre-VOID END. TEST DEFECT (VERIFIED)**
- Counts: pins latch.END 1 (V4), before and after step 1. With `T_END`, the pins scope is fully green (VERIFIED).
- What the product writes in V4 (VERIFIED, `DE_MTRACE` pin trace; the model's timeline is identical):
  `OPEN@0 (initial, plain) -> [AB, TERM18, EXIT18 receipt: nothing] -> at S(19) L boundary: DATA EXIT18 -> END@18 (plain) -> FRONTIER 18 rejected by sink -> OPEN@0 (plain, revoked=0) -> VOID(writer_push_fail)@20 -> REARM -> TERM18 (nothing) -> F19 (epoch 2)`.
  Final state is `x=0, closed=0, ENDs=1, revoked OPENs=0`. This is exactly E:90 ("Combined publication timeline: `OPEN → EXIT_D18 → END18 → OPEN → VOID20[Cw,writer_push_fail] → F19[Cw]` ... the second OPEN is successfully persisted before the resulting VOID callback begins. Both OPENs are plain").
  Product code: cap:276-290 publishes the plain rollback OPEN when `startup_prepared` before the VOID reaches the sink.
- What the invariant observed: host:474 runs the latch checks once the faulting primitive publishes its first VOID, comparing against the state before the whole primitive (`r->previous`). Then host:484 sees ENDs 0 -> 1 and reports "successful startup END after first VOID". That END was published *before* the first VOID, in the attempt whose failure produced it, and it never committed.
- Contract: M:474 "END publication is not commitment ... If the attempt fails after its END was successfully published, that END never committed: before publishing that failure's VOID, successfully persist a startup OPEN ... Historical successful DATA and END publications remain observable". M:504 latch: "no new ... startup-gap END commits" after the first fault.
  The product is right. The invariant is wrong. The driver's own `views.VOID_failed_END` check (host:493-501) already covers this case correctly and passes.
- Probe fix: when no VOID came from an earlier primitive, count only ENDs that follow the first VOID in the publication timeline. The self-check fake `latch_END` is still caught (it uses `saw_void=true`).

## Class x cause (failing descriptors, baseline = full.log; "T1+T2" = both needed)
| class (scope) | total | T1 | T2 | T1+T2 | G1 | T3 |
|---|---|---|---|---|---|---|
| model.DATA (enum) | 22,052 | 21,914 | | | 138 | |
| model.FRONTIER (enum) | 73,444 | 23,782 | 48,748 | 848 | 66 | |
| model.VOID (enum) | 73,819 | 23,782 | 49,030 | 848 | 159 | |
| model.epoch_invalidation (enum) | 54 | 54 | | | | |
| model.report (enum) | 73,729 | 23,782 | 49,030 | 848 | 69 | |
| model.fault_diagnostics (enum) | 73,819 | 23,782 | 49,030 | 848 | 159 | |
| model.publication_order (enum) | 73,819 | 23,782 | 49,030 | 848 | 159 | |
| 8 H2a pin classes (pins) | 1 each | 1 each | | | | |
| driver.H11c_continuity_unrepresentable (pins) | 1 | 1 (encoding) | | | | |
| latch.END V4 (pins) | 1 | | | | | 1 |

No PRODUCT DEFECT was found: every enumerated and pinned difference is explained by T1, T2, T3 (test) or G1 (contract gap).

## What I could not determine
- G1 is a reading choice; no row pins AD19, TERM18, EXIT18. Someone has to decide whether a TERMINAL control with `x<e` refutes the admission proof at TERM time.
- T2 assumes R:221's "D's continuing attachment" rule applies equally to H in the enumeration-only H1 prologue (INFERRED). The H1 prologue is not a contract row.
- Latent, not exercised (INFERRED from reading): model:161-162 faults missing continuity only on a TRANS in the batch. mc:1043 faults on any record of the admitted lifetime, including an EXIT.
  M:504 says "at that lifetime's first eligible-batch validation", which supports the product. Only H11c sets missing continuity and has no EXIT, so the enumeration cannot see this.
- I did not run the product's own oracle tier (`tests/discovery_oracle_assertions.txt`), any sanitizer build, or any box tier.
- Scratch worktrees `W/base`, `W/fix1` and `W/fix2` are registered in the main repo's worktree list. Remove them with `git worktree remove --force <path>` when no longer needed.
