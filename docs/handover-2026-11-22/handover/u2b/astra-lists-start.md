INFERRED — Choose **(i)**: preserve both bounds and make their scopes explicit. The startup example represents boot-wide loss evidence and its window-clipped projection, not conflicting measurements. The defect is the promise that every inventory interval lies wholly inside “All”; that promise must be corrected explicitly, not marked passing.

VERIFIED — At `b15ec1d2`, `src/server.c:5431–5460` builds lists separately for every boot, starting at `boot_start`; `src/coverage_trace.c:433–470` selects the latest boot’s retained start and extends its default end through witnessed loss. These are different temporal populations.

VERIFIED — `/private/tmp/u2b-r18-evidence/startup-gap-list-replies.json:16–52,67–72` records All `[10005s,10022s)`, inventory gap `[10000s,10022s)`, and window loss `[10005s,10022s)`. Both saved advisers left this unresolved under their implementation scope (`astra-lists-advice.txt:1–3`; `fable-lists-advice.txt:9–12`, same directory).

INFERRED — Exact guarantee: **“For a successfully evaluated, single-boot default snapshot, every listed gap and hole ends at or before All’s end T, and its nonempty intersection with All’s interval [S,T) appears in the default coverage.uncovered with the same reason; inventory intervals may begin before S.”**

VERIFIED — The single-boot qualification matters: lists enumerate every boot independently (`src/server.c:5431–5452`), whereas the default uses the latest boot (`src/coverage_trace.c:433–434`); the combined verdict also resolves overlapping boots and reason precedence (`src/coverage.c:411–434,574–581`).

INFERRED — Do not publish that guarantee universally across boots or clock generations without separate proof. Earlier boots can have different wall-clock ranges, and their isolated verdicts need not equal the combined window verdict. Document those inventories separately from the selected window.

INFERRED — Add an independently specified `startup_loss_before_retained_start` row: boot start `10000s`, S=`10005s`, loss `[10000s,10022s)`, T=`10022s`. Assert exact inventory and uncovered endpoints, unchanged S, incomplete metrics withheld, and successful replay with the loss explanation. Require mutants clipping the inventory at S, moving S backward, or shortening T to fail their respective assertions. Add an analogous missing-file row.

VERIFIED — Existing containment assertions are at `tests/test_coverage_consumers.py:588–590,737–739`; fixture creation defaults its initial observation to its start (`:377–384`). The saved startup counterexample exercises the missing distinction.

INFERRED — API documentation should say: “`gaps` and `holes` are boot-wide diagnostic intervals derived from retained evidence, using the DB coverage floor; they are not restricted to the response’s selected window and are not a complete history of discarded evidence. `coverage.uncovered` describes the selected window. All begins at the latest boot’s retained start.” Label any inventory display “Boot-wide capture losses / missing files”; explain that it may include time before All.

INFERRED — External consumers receive unchanged fields, endpoints, and reasons under (i); the documentation corrects their interpretation. Option (ii) discards the inventory’s earlier extent. Option (iii) changes reason precedence and explicit-window answers, which is a separate semantic decision.

VERIFIED — SEARCHED: `git grep -n -E 'gaps|holes' b15ec1d2 -- web/static ':!web/static/vendor/*'` returned fixture-local variables and comments, no info-list reader. The UI reads info bounds (`web/static/lib/state.js:78–86`) and window uncovered ranges (`web/static/lib/builders/fidelity.js:168`). Replay obtains the window verdict directly and handles incomplete coverage successfully (`src/replay.c:148–174`).

INFERRED — No current or planned consumer requiring the boot-wide start was identified in the searched UI, replay, rework plan, and tag-checklist material. NOT SEARCHED: external consumers and unprovided plans. A future diagnostic view could use it; displaying it as loss “inside All” would be misleading. The checklist should require the new exact row and demonstrated mutant failures before certifying the revised guarantee (`docs/TAG_CHECKLIST.md:16–24,53–55`).