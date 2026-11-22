INFERRED — Choose rejection on delivery for question 1. Finish the bounded merge corrections, and let coordinator development build on them. Treat publication ambiguity and failed retraction as separate storage obligations before production enablement.

VERIFIED — References below use `C=wt-m12r/contracts/merge-v1/CONTRACT.md`, `R/E=its oracle-discovery/ROWS.md/EXPECTED.md`, and `Review=a2go/disc/review-merge-003ee35a.md`; source paths refer to `wt-m12r`, relative to the supplied scratchpad.

INFERRED — **1. Move the general rejection rule into Decision 40**, leaving Decision 36’s enable/termination exception explicit: “In every admission state, reconcile a terminal control against all retained evidence, including its named cookie-less EXIT, before recording a final terminal claim or disposition; reject a control already contradicted on delivery, retain independently established identity and refutation history, and do not treat that rejected control as a retained final claim against which a later otherwise qualified control conflicts.”

VERIFIED — This resolves the ambiguity between C:360 and C:460 consistently with C:504’s prohibition on recording an accepted disposition for a rejected control. H6c remains distinct: its first claim precedes the contradicting evidence (`R:250`, `E:198`).

INFERRED — Add a row `EXIT_D18@L, TERM_D19{names that EXIT}, TERM_D18`, without ADMIT: accessors after successive inputs are `NONE, NONE, MATCHED(18)`. TERM19 produces the sole `VOID20[C0,unknown_evidence]`; TERM18 records an otherwise unrefuted claim without another fault. Final state: suspended, latch set, census known, pending `1+0`, no bound/discharge/END, startup gap open.

INFERRED — Add the cookie-less version with identical accessor and fault expectations. Compare TERM19 with the retained raw EXIT before recording MATCHED; the adapter’s delayed delivery to the core must not change the answer. Retain independently proved binding and the disposition obligation through payload disposal. A subsequent finish after TERM18 adds no missing-disposition fault.

INFERRED — Add `D20@P0, TERM_D19, RA, TERM_D21`, both controls naming the same prospective EXIT: accessors `NONE, NONE, NONE, MATCHED(21)`; only `VOID20`, with the epoch active after RA, certification latched, pending `1+0`, no discharge or END. Retained D20 contradicts 19 but not 21. Preserve H6c’s two faults and historical `MATCHED(19)` because its first claim was recorded before D20. Pin rejected retransmission idempotence separately.

VERIFIED — Merely removing the core’s admission-related gate is insufficient for the cookie-less case: capture checks core-retained EXIT evidence before recording the disposition (`src/merge_capture.c:1048–1059`), while the review observes `MATCHED(19)` for the held cookie-less EXIT (`Review:178–180`).

VERIFIED — **2. S2 is a test gap in the current implementation.** The lost discharge occurs under mutant my8; current `merge.c:1183` calls the terminal-only path and T51 succeeds (`Review:64–70`). Decision 41 permits the current terminal candidate backed by a previously completed EXIT release (`C:480`); G8 explicitly commits such a discharge while another batch is held (`E:169`).

INFERRED — Add T51 as a row: census `{D,H}`; `EXIT_H18, S19, D20, S25, TERM_H18, AD_D6, S30`. Expected combined timeline: `OPEN → EXIT_H18 → F18 → D20[U] → F24 → END21 → F29`. TERM commits `x_H=18`, pending `2→1`, without releasing D20. AD commits `b_D=20`, `C_close=20`, `P_close=21`, pending zero. Census known, latch clear, zero faults; no held prospective bound contributes at TERM.

VERIFIED — That timeline and state are recorded in `rv18/logs/p-T51.log:22–63`; losing the discharge would violate the stated behavior, despite erring toward refusal.

INFERRED — **3. S3 is a product defect under existing text.** C:144 requires suspended payloads to be disposed of without surviving for later release; C:444 says the recorded MATCHED satisfies the final-disposition obligation. A disposed EXIT cannot remain an unfinished record merely because the staging counter was incremented.

VERIFIED — The staging increment is at `src/merge_capture.c:1517–1518`; finish tests that count through `waiting()` (`:1664`, `:1706`). The review records unfinished count 1 and the extra VOID after REARM (`Review:72–84`).

INFERRED — Strengthen H11f to pin pending payload records zero after disposition and unfinished records zero at finish; its unresolved inventory count remains one. Add a separate variant inserting valid `REARM(D proof), S26` before finish: expected stream `VOID20 → F25`, one unknown-evidence fault, MATCHED retained, startup gap open, latch set, and no VOID40.

VERIFIED — **4(a). Persisted END plus a false return violates the publication interface’s meaning.** C:474 expressly makes a complete reader-visible END the commitment. The XA probe nevertheless leaves the capture open while `[6,7)` reads complete (`rv18/logs/p-XA.log:10–17`).

INFERRED — Thus this is a capture/file disagreement requiring correction; the file’s END is not intrinsically false when all supporting evidence succeeded. Under Decision 41, the capture must recognize that commitment. A later writer failure may suspend further output, but cannot retroactively turn that committed END into a rejected publication.

INFERRED — The smallest rule is: “An END publication result reports whether END became reader-visible; a committed END cannot be reported as uncommitted, and subsequent failures are separate observations.” Put fallible supporting work before END and distinguish committed, definitely uncommitted, and indeterminate outcomes where necessary. An indeterminate outcome needs conservative reader treatment, not a rollback write assumed to succeed. Exposure is the reader-eligible interval after `P_close`, potentially much larger than XA’s demonstrated cell.

VERIFIED — The real publisher calls another fallible publish after appending the gap (`src/open_snapshot.c:662–666`); the coverage writer already flushes and advertises its block (`src/event_writer.c:1026–1033`). These reads identify the interface boundary to test; they do not establish a reproduced production failure.

VERIFIED — **4(b). Failed retraction can leave a false completeness claim relative to established capture evidence.** Decision 43 explicitly acknowledges it (`C:506`). The probe retains `[6,19)` and reaches `[6,30)` after replay REARM (`rv18/logs/p-V6.log:22–50`).

INFERRED — This exposure is historical: potentially the entire otherwise eligible interval from `P_close` to the last persisted coverage endpoint, with no fixed duration bound. Stopping further watermarks bounds growth but does not retract that interval. A prefix containing neither retraction nor failure evidence is indistinguishable from the same prefix before the contradiction.

VERIFIED — Existing sealing/liveness rules do **not** reject all such history. The reader uses `W` while live and the retained floor when stopped uncleanly (`src/coverage.c:383–397`); `unclean_end` starts at that endpoint (`:544–545`), and COMPLETE does not require sealing (`:624–627`). Earlier windows may therefore remain complete.

INFERRED — A sufficient conservative reader rule is: discovery-dependent windows cannot be COMPLETE or LIVE_EDGE without a trustworthy final seal establishing completed reconciliation and no unresolved publication/retraction failure. Unsealed discovery results must remain unavailable or explicitly provisional, including while live. Discovery participation must be durably identifiable; otherwise apply the gate to a broader identifiable boot class.

VERIFIED — That rule also needs a writer obligation. The exact-perf tick returns success without advancing after unhealthy capture or `gap_failed` (`src/open_snapshot.c:532–533`), yet the seal routine can subsequently write SEAL (`:582–587`). The existing CLEAN finalization checks storage operations (`src/event_writer.c:1203–1226`); it does not by itself certify discovery reconciliation.

INFERRED — Require a sticky publication/retraction failure to prohibit a usable final seal, including after recovery or restart; test crash-before-retraction and failed-write shutdown. Tell the owner explicitly: “END-last prevents premature closure within an attempt, but cannot make later retraction atomic. A crash or failed retraction can leave historical discovery windows falsely complete indefinitely; current liveness and clean-close handling do not eliminate that risk.” Production enablement needs the conservative protocol or an explicit owner decision on this exclusion.

INFERRED — **5. Accept the proposed bounded merge round with two adjustments:** obtain fresh independent answers for new **and amended or semantically affected** rows; and include cookie-less reconciliation in question 1’s implementation, not just the core gate. Fix S1’s actual EXIT position, add T51 and H11f/REARM assertions, cover the committed-state END route, and make enumeration follow the chosen rule.

VERIFIED — S1’s timestamp mutant survives because the driver names the wrong EXIT position (`Review:50–62`); the committed-state END route lacks a crash-cut execution (`Review:90–94`). The method requires independent expectations and invalidates dependent claims when their contract changes (`docs/REWORK_PLAN_2026-11-11.md:64–67,190–192`).

INFERRED — After those checks pass, the merge semantics are sufficient for coordinator development. Neither storage issue warrants stopping otherwise authorized producer/coordinator work now, but both constrain production readiness. Give the storage protocol its own scoped contract and implementation work; do not silently expand the one-round merge budget or describe production discovery as complete while these obligations remain.