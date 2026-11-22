I accept the challenger’s narrower continuity proposal and conservative exit check. This supersedes my earlier request for header members and P2 continuity implementation.

1. **Continuity:** preserve section A’s header declarations. At `003ee35a`, `merge.c:894–895` needs the input only beyond epoch 1; REARM advances the epoch and replaces attachment proofs (`1575–1591`). Production REARM remains gated by the test premise (`merge_capture.c:1769,2106–2113`). Waiting therefore breaks no enabled production recovery path. Post-VOID recovery of a discovery capture is not implemented; production REARM is off.

   The unit enabling production REARM must specify and qualify an accessor-only per-binding witness, acquired on demand at the REARM request and delayed-ADMIT delivery. It must compare the kernel perf event ID retained at original enable with the current ID, match the original binding/generation/address, establish a nonterminal pinned task and account for disable/close/reopen operations and applicable loss. The numerical descriptor belongs to the backend (`566f20d5:discovery_producer.c:1366,1414`); this requires cooperation with that owner. Route c additionally needs the original pointer baseline and a completed observation covering the relevant barrier, even when unchanged. Event-ID equality alone does not prove uninterrupted enablement. A prior refresh stamp is insufficient; intervening work requires renewed evidence. Unknown observations remain unknown; only demonstrated interruption is known false. This witness belongs to the production-REARM unit, not P1 or the currently specified P2.

2. **S3 at exit:** accept the conservative close. The observer map is keyed by a reusable PID (`discovery_producer.c:1055–1059`), so the current interfaces cannot supply an independent final lifetime-bound observer total. Compare the retained event’s final perf count with the last validated snapshot’s absolute `fires`. Equality closes that observation tail; a greater or unreadable count leaves it unknown and produces one user-evidence increment plus a scoped FAULT. Missing baseline, decreasing count or unqualified wrap likewise cannot close it. None demonstrates a continuity break. One EL8 probe must establish that the retained FD returns the final count after exit; otherwise state that limitation and use the unknown outcome.

3. **Sticky fault:** P1 counts the transition into the fault latch once and preserves readable cumulative totals. P2 moves mandatory observation/comparison ahead of the history-success gate and supplies bounded, allocation-independent inspection and delivery. Repeated ticks do not recount the latch; independently detected later faults still count. Until inspection is available, incomplete reconciliation cannot qualify a live route-c proof as clean.

SPEC DELTA 1

**spec-frozen.md, A. REPLACE:**

> All other existing function signatures remain unchanged. Record totals and reconciliation cuts qualify that record’s historical census evidence. The fresh checkpoint gates present acceptance and exposes intervening invalidations; newer totals cannot upgrade an older census cut.

**WITH:**

> All other existing function signatures remain unchanged. Record totals and reconciliation cuts qualify that record’s historical census evidence. The fresh checkpoint gates present acceptance and exposes intervening invalidations; newer totals cannot upgrade an older census cut.
>
> Re-issue CENSUS in place at cursor zero. Its establishing cut is the completed reconciliation pair current when the whole census predicate first evaluates true, including resolution of every numeric obligation and complete handoff of the required evidence. Capture the supporting record-local accounting with that proof. Freeze the establishing pair and its proof evidence while the census remains proved; unrelated successful refreshes do not move it forward.
>
> Invalidation clears `census_fenced` and retains the last successful pair without claiming current success. A transient fence failure leaves the census pending. Re-proof requires a strictly larger completed reconciliation ordinal; an ordinal once returned unproved must never later be returned proved. Do not manufacture an ordinal without completing reconciliation. In particular, a number resolved at a later cut cannot establish census completion at the earlier scan cut. Current checkpoint totals must not overwrite historical census totals.

**spec-frozen.md, A. APPEND AFTER the exact sentence:**

> Reject contradictory tags; zero payload unrelated to the record’s kind.

**TEXT:**

> A failed-open number subsequently bound to a qualified FORK or NONMEMBER holder produces its NUMERIC record followed by exactly one RESOLUTION R, with a retained nonzero binding token, all four qualified nonzero task IDs, the validated fork cookie when applicable, and discovery cookie zero. Reserve no discovery cookie and emit no INVENTORY for that holder; retain its binding through the resolving fence and handoff. Preserve independently established earlier journal lifetimes.
>
> Classification must precede ordinary inventory publication, including the scan-time fork-provenance path. Every schedule must preserve `!(excluded && inventory_reported && !kernel_only)`, and census `inventory_count` must equal the live INVENTORY obligations handed over. Never repair disagreement by hiding an already published obligation; a later contradictory classification must retain that obligation and report the uncertainty.

**spec-frozen.md, A.1. APPEND AFTER the exact sentence:**

> Full finality reporting, retained inspection and shutdown behavior belong to P2.

**TEXT:**

> The P1 correction classifies fork-born and uninventoried backends before applying the stop gate: those routes retain `-ENOENT`; only a matching discovery attempt is refused by the gate. A pointer enable that never succeeded reports `pointer_continuous={false,false}`, preserves its positive errno and existing failure accounting, and does not by itself end attempts. A demonstrated break of an established attachment remains sticky.
>
> P1 also closes the terminal pointer-observation tail before discarding its retained event. After the task/event is quiescent, read the final perf count before closing the FD. A readable, qualified nonwrapping count equal to the last validated snapshot’s absolute fires closes only that tail and clears no earlier fault. A greater or unreadable count, missing validated snapshot, or invalid count ordering leaves continuity unknown and latches one user-evidence increment plus a lifetime-scoped FAULT; it must not manufacture a demonstrated break. Repeated inspection of the same unresolved tail does not recount it. Qualify the final-count read after exit with one EL8 probe; if unavailable, state that limit and retain the unknown outcome.
>
> P1 accounts for the first transition into a sticky producer fault once, without counting the same failure again merely because it also set the latch. Readable cumulative evidence totals remain known; the latch alone does not make them unreadable. The conservative P2 API stubs remain unchanged. P2 owns moving mandatory pointer inspection/comparison out from behind successful history reconciliation, bounded refresh, allocation-independent retained fault delivery, and complete shutdown ordering before merge finalisation and storage destruction.

**spec-frozen.md, B. APPEND AFTER the exact sentence:**

> Copy identities from retained lifetime evidence, never the current PID map.

**TEXT:**

> Set `current_epoch_continuity=false` and include no discovery lifetime in REARM attachment proofs until the unit enabling production REARM supplies an independently qualified per-binding witness covering the original startup attachment through REARM and the current observation.

**spec-frozen.md, B. REPLACE:**

> | POINTER | `pointer_read_ok=pointer_snapshot_valid`; `pointer_value=(pointer_nonnull!=0)` is Boolean, never an address; actual fire time unavailable means `pointer_time=0`. Supply continuity only when known; do not encode unknown as a demonstrated break. Retained contradictions precede claims/release. |

**WITH:**

> | POINTER | When `pointer_continuous.known=false`, emit no POINTER control; retain diagnostics and process applicable evidence increments or scoped faults independently. Otherwise set `pointer_continuous` to its known value, `pointer_read_ok=pointer_snapshot_valid`, and `pointer_value=(pointer_nonnull!=0)`; the latter is Boolean, never an address. Actual fire time unavailable means `pointer_time=0`. Retained contradictions precede claims/release; suppressing an unknown-continuity POINTER must not suppress independently established faults. |

**spec-frozen.md, C. APPEND AFTER the exact sentence:**

> Drain available handoff before classifying discovery DATA; stage records whose origin is not yet provable, preserving source order and capacity accounting.

**TEXT:**

> Track the producer’s census ordinal and whether it has been observed unproved separately from coordinator acceptance. Refuse an unproved-to-proved transition at the same ordinal and do not apply that positive census. Apply an eligible positive census at most once per ordinal, after dependent obligations, negative updates and faults. A checkpoint-gated deferral of an otherwise proved record is not itself a producer-unproved observation. Repeated proved records must retain the same establishing cut and proof evidence; re-proof after invalidation requires a larger completed ordinal.

**spec-frozen.md, C. APPEND AFTER the exact sentence:**

> Inspect sticky scoped contradictions independently of history.

**TEXT:**

> A sticky fault contributes one evidence increment at its latch, never one per polling tick; later independent faults remain separately accountable. Known cumulative totals remain known unless their own reads or arithmetic fail. Mandatory count inspection and qualified perf-versus-observer comparisons continue despite the latch or failed history; unavailable prerequisites yield unknown evidence, never a clean comparison. Until P2 supplies that inspection path, a live route-c proof with reconciliation other than COMPLETE has unknown current continuity and cannot justify clean release or certification. Preserve demonstrated breaks independently of that unknown state.

**P1 FIX ROUND**

Freeze this clarification and its acceptance cases first. After the qualification round, perform one producer correction round:

1. **B1 — separate numeric binding from discovery inventory.** Implement cookie-zero R handles for FORK/NONMEMBER holders and correct both `1709–1733` and the scan classification site at `1817–1823`. Host cases: failed open followed by each holder class; fork provenance first recognised at the second site; repeated refresh; preservation of an earlier journal lifetime. Assert NUMERIC then one qualified RESOLUTION, no holder INVENTORY/cookie, retained nonzero token, the exclusion invariant and matching census inventory count. Mutants restoring unconditional `bind_inventory` publication or post-publication exclusion must fail those assertions.

2. **B2 — establish and retain the correct census cut.** Host case A: scan fence fails, later refresh completes; the first proved census carries that later nonzero completed pair. Host case B: scan completes with a pending number, then G resolves at a later cut; census completion carries the establishing cut covering G. Extend both with unchanged refreshes, invalidation, re-proof and retained copies of earlier results. Mutants retaining the scan proof, flipping false to true at the same ordinal, moving a still-proved cut forward on every refresh, or overlaying current totals must fail the corresponding assertions.

3. **S1 — put classification before the stop gate.** After stop, fork-born and uninventoried backends still return `-ENOENT`; a matching inventoried discovery backend is refused and performs no new physical attempt. Preserve existing scan/retry guards and required watches. The mutant restoring the early unconditional stop refusal must fail the fallback case; removing the matching-entry gate must fail the no-attempt assertion.

4. **S2 — unknown after unsuccessful initial enable.** Inject EIO on the first pointer enable, then permit a retry. Assert unknown continuity on the failure, retained errno and accounting, no invented attempt finality, and no demonstrated-break latch. Include an established-attachment break that remains sticky. Mutants restoring `fact(false)` on the initial failure or clearing a real break must fail. The successful retry does not erase the earlier fault or restore certification already latched off.

5. **S3, exit half — final retained-event count.** Host cases: final count equals the last validated fires; a missed fire advances it before exit; final read fails; no validated snapshot exists; repeated teardown inspection; PID reuse replaces the observer-map value. Assert clean tail closure only for qualified equality, otherwise one increment and scoped unknown-evidence FAULT, with no invented break. Mutants restoring the terminal early return, skipping the final read, accepting a replacement’s observer count, or classifying the unknown tail as a break must fail. Run the single EL8 final-count probe in qualification; no BPF changes. Delivery across failed history/append and full shutdown sequencing remain P2.

6. **Sticky-fault accounting, P1 half.** Inject a real producer operation that first sets the sticky latch, then repeatedly refresh/read retained evidence without another failure. Assert one increment for that fault, stable known totals and preserved invalidation; inject a distinct later fault and assert separate accounting. Mutants omitting the latch increment, recounting it each tick, or clearing count validity merely because the latch is set must fail. Keep transitional checkpoint/binding stubs conservative. Moving comparisons ahead of the history gate and allocation-independent delivery remain P2.

All mutants must build, reach the intended case and fail its intended assertion. Preserve existing registered checks, header identity, guards and independent answers.

**COORDINATOR**

1. **Census ordinal gate:** implement the section C rule. Exercise both B2 schedules, repeated positive reads, true→false→true at one ordinal, valid re-proof at a larger ordinal, and acceptance deferred solely by the fresh checkpoint. A mutant accepting the same-ordinal false→true flip must fail. Assert that a later numeric resolution cannot close startup coverage using the earlier cut.

2. **Unknown POINTER suppression:** an S2 failure record produces no POINTER control while its errno/evidence fault remains handled. Add a known-break control case so suppression cannot erase a real contradiction. The mutant coercing unknown to Boolean false must fail. Apply the same treatment to S3’s unknown tail; incomplete current reconciliation cannot preserve a clean live route-c assertion.

3. **Section B continuity rule:** leave the ADMIT input false and discovery lifetimes out of REARM attachment proofs. Inject an explicit REARM in host tests: otherwise valid historical ADMIT remains recorded, and missing continuity faults at the first eligible batch before publication. Cover both already-recorded and delayed ADMIT. Mutants inventing continuity from historical admission or populating REARM proofs without a witness must fail. I1’s next correction consumes this delta; I2 need not wait because generation B, cookies and terminal identifiers are unchanged, subject to file-ownership checks. Production wiring still requires P2.

**NOT DECIDED HERE**

Production-REARM witness declarations and qualification; kernel-initiated interruption observability; D3 role/publication choices; BPF or persistence changes; attribution of the EL8 run-7 failure; broader live qualification, performance costs and unrelated review nits. This correction must not be reported as resolving those items.