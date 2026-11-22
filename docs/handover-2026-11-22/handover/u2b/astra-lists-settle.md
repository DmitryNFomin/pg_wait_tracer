INFERRED — I withdraw (i). Choose **(iii) plus the next-boot stop**, in a separate specification unit. The prototype establishes a useful minimum repair; it does not establish its proposed universal guarantee.

VERIFIED — C is an independently demonstrated contradiction: All `[130,200)` is complete, while `gaps` contains `[55,200)`; explicit `[140,150)` is also complete (`scratchpad/u2b/fable-lists-probe/twoboot.log:1–3`). An API consumer intersecting that gap with All would mark the entire window lost while coverage permits metrics.

VERIFIED — The stated master premise is false at this check. `git rev-parse origin/master` returned `47370c9f09ef41ea3157422fe82487e188eea602`; `git grep -n cov_info_lists origin/master -- src/server.c` returned no match. GitHub `github_fetch_file(..., path="src/server.c", ref="master")` returned blob `602f7ddc624f4d7fdabac226802e5a73338f28a0`, identical to the local master blob, with no `cov_info_lists` or `coverage_trace` matches. SEARCHED: both complete server files; NOT SEARCHED: deployed binaries and other branches.

VERIFIED — On `b15ec1d2`, each inventory query isolates one boot and extends its end using the request and later observations (`src/server.c:5442–5452`); the window evaluates the combined model (`:5337`). Thus C is demonstrated on the review branch, not demonstrated as a shipped master defect.

VERIFIED — A defeats the “millisecond overhang” framing: inventory `[0,420)` versus All `[400,1000)`, with explicit `[300,400)` labelled `capture_loss` (`probe.log:2–6`). The prototype changes that prefix to `retention`; F changes its prefix to `not_captured` (`candidate.log:10–16`). These are semantic changes, not documentation corrections.

INFERRED — Amended precedence: **“For each boot b, its capture-loss and missing-file reasons apply only at or after S_b. Within [boot_start_b,S_b), its leading-absence reason is retention when leading files were removed, otherwise not_captured. Other boots and clock generations remain subject to the combined coverage rules.”** Do not globally promote retention over evidence belonging to another boot.

INFERRED — Required guarantee: **“For one evidence snapshot, evaluation time and DB floor, intersecting info’s gap/hole inventory with any advertised window yields exactly that window’s capture_loss/missing_file uncovered ranges, after clipping and coalescing. No listed loss overlaps an interval certified complete.”** Historical entries may remain outside All; documentation must state their scope.

VERIFIED — The prototype stops an earlier inventory at the next boot’s **retained start S**, through `pgwt_cov_boot_start`, not necessarily its raw `boot_start` (`both.py:10–12`; `b15ec1d2:src/coverage.c:48–50`). C becomes `[55,130)` (`both.log:1–3`).

INFERRED — Do not claim the guarantee follows from those three lines alone. An earlier open loss remains active in the combined sweep after that inventory stop; where a later boot has a hole, its `missing_file` can lose to the earlier `capture_loss` (`b15ec1d2:src/coverage.c:546–548,578–581`; `src/coverage.h:80–89`). Add this concrete adversarial row; resolve inventory and window reasons through the same combined authority if it fails.

INFERRED — Required rows: **F:** gap `[5,22)`, explicit `[0,10)` = not_captured `[0,5)` + loss `[5,10)`; **A:** gap `[400,420)`, explicit `[300,500)` = retention `[300,400)` + loss `[400,420)`; **C:** historical gap `[55,130)`, clean All `[130,200)`, default metrics available. Assert exact endpoints, unchanged default bounds, and list/window equality.

INFERRED — Also require missing-file counterparts, unequal DB/idle floors, next boot with `boot_start < S`, earlier open loss plus later hole, and overlapping clock generations. Removing the S clamp must fail A/F; removing the boundary stop must fail C. The combined-model row must detect residual reason disagreement.

VERIFIED — Saved evidence reports 58 windows plus 17 rule cases passing for the clamp, and 29 consumer cases passing for both changes (`candidate.log:1–2`; `both.log:5`). Those are saved prototype results, not fresh validation or proof of the additional rows.

INFERRED — Keep this outside round 18’s preserved-verdict scope (`/private/tmp/u2b-r18-evidence/scope-questions.txt:1`). Review the contract/specification delta first; obtain two independently derived answer sets and freeze them before implementation, consistent with `docs/REWORK_PLAN_2026-11-11.md:60–67`.

INFERRED — **The coverage-label unit cannot merge first with these lists unchanged.** Lack of a UI reader does not remove the contradictory API reply. Fix the separate unit before shipping the combined result, or obtain an explicit owner exclusion; documentation, green tests, and “already on master” cannot substitute for that exclusion (`CLAUDE.md:24–28`).