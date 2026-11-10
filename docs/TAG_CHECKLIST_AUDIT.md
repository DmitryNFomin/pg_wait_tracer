# Tag checklist v1: inventory and evidence audit

The first 82 IDs correspond in order to the Astra inventory supplied with this task.
Those citations were inspected against this checkout, not accepted as passing results.
No missing adviser file or explicitly named suite/function was found (0 such corrections).
Two source-location citations were refined to actual Node test declarations: adviser row 22
(`table.test.mjs:1490` -> `:1483`) and row 24 (`:1511/:1520` -> `:1509/:1518`).
Ellipsized Node titles and assertion-only citations were expanded into exact titles or named
script suites. `test_data_transitions` and `test_data_lock_chains` are top-level TestRunner
suite names, not Python functions; the script suite is the selector for their module assertions.
`test_cross_validate_tiered` is the shell suite name. Shell helper selectors name existing
functions in the invoked suite; v1 observes the suite's step, not individual assertion records.
`test_inventory_citations_and_ci_registration` checks every final file/name and CI binding.

The original coverage claims needed scope changes even where their citations existed:

- L001/L013/L015/L021 are canned-display checks, not production numerical oracles.
- L002: actual duration tolerance is 0.1% relative and percentage tolerance is 1% relative.
  L003 uses 1% components and 2% off-CPU/partition bounds; L006 permits 5% relative error.
  The source docstrings saying “0% tolerance” are not the assertions.
- L005/L083 split IO-worker versus CPU-sample cases with different bounds (2%, 5%, 6%).
- L012/L084 split server percentiles from their browser rendering; the server permits 1%.
- L017/L026 retain uncovered Sessions percentages and end-to-end query latency populations.
- L021/L085 split the browser percentage from builder empty-result disclosure.
- L029/L086 split the histogram builder from the aligned-grid C route comparison.
- L031/L087 split exact server timestamps from mock-browser geometry (<1 s raw duration error).
- L040/L088, L041/L089, L046/L092, L048/L093, L049/L094 and L051/L095 split evidence steps.
  L089 is only >=9/10 occupied buckets, not an exact burst or peak oracle.
- L043/L090 distinguish fixture-inferred candidates from real PostgreSQL lock identity;
  L044/L091 distinguish positive overlap/score presence from independently correct values.
- L054/L096 and L055/L097 split server capability declarations from mock rendering.
- L056/L098 split mock controls from live escalation; L057/L099/L100 distinguish rendering,
  live numeric-presence checks and the still-uncovered independent counter oracle.
- L058/L101 distinguish estimated-overhead formula checks from measured accuracy.
- L064/L102 split browser chart reuse from the separate chaos CI step.
- L068/L103 and L070/L104 split active-session/live CLI and C replay/dump evidence.
- L074 covers query-filtered cross-tab raw-route agreement (oracle relative error 1e-7,
  spread <=0.001 ms), not general unfiltered raw/summary equality.
- L075 retains uncompromised raw/summary agreement without a test. L148 is the existing
  diagnostic with #410 expected differences; it cannot prove that stronger guarantee.
- L078 states +/-8.8 pp. L079 states the 0.3–3.0 different-window DB Time ratio;
  L145–L147 retain the missing exact-window CLI/server equalities.
- L080 does not establish an EL8 kernel/UI/docs contract. L081 and L106–L117 preserve
  that native matrix; PG16 fatal-refusal CI is not evidence of “never fails”.
- L082 proves synthetic keys only; L105 preserves missing PG13 graceful startup coverage.
- L149/L150 disclose existing 15% sampled and <50% full throughput budgets, separately
  from the unbudgeted owner-aligned capture-mode obligations L151–L154.
- L171–L173 retain uncovered end-to-end execution duration, timestamp and P95 oracles.
- L155 preserves realistic workload; L156–L158 distinguish live first paint, uncovered
  interactions and a replay API latency bound. L159–L170 retain per-view recovery coverage.

Evidence bindings were established from `.github/workflows/*.yml`, `tests/Makefile`,
`tests/unit_tests.list`, `tests/run_all.sh`, `tests/ci_smoke.sh` and `scripts/check.sh`:

| Binding | Actual invocation | Scope |
|---|---|---|
| ci.yml / build-and-unit / Run C unit tests | make -C tests check, unit_tests.list | Hosted Ubuntu replay/compute tests; not native PG matrix |
| ci.yml / build-and-unit / Run synthetic data tests | tests/test_data_*.py loop | Hosted Ubuntu generated traces |
| ci.yml / build-and-unit / Run web UI builder unit tests (Node, no browser — Phase B3) | node --test tests/web_unit/*.test.mjs | Builder fixtures |
| ci.yml / build-and-unit / Bridge integration test (Go client ↔ real pgwt-server — T6) | go test -race ./..., PGWT_BRIDGE_TEST_REQUIRE=1 | Real replay server, no live PG |
| ci.yml / web-ui / Run web UI tests (mock server) | tests/test_web_ui.py | Mock transport and canned data |
| ci.yml / web-ui / Run web UI chaos tests (adversarial mock — Phase B2) | tests/test_web_ui_chaos.py | Adversarial mocks |
| ci.yml / sampled-overhead (PG 17) / Resolve gate-box PostgreSQL port + bpftool, build, and gate sampled overhead (single lock hold) | sampled_overhead_gate.py | Ubuntu kernel 6.8, PG17; conditional step |
| box-check | run_all.sh direct entries, LIVE_TESTS, or its live UI harness | NOT-RUN in v1; no typed box results |

Capture smoke CI invokes ci_smoke.sh, not run_all.sh. Its optional watchpoint sections
can skip inside a successful step, so none is used here to certify full-mode guarantees.
Nightly container jobs share Ubuntu's kernel and do not certify native EL8 4.18 or EL9.
The v1 step model cannot establish per-case execution, internal skips, platform attestation
or numerical oracles beyond the scoped assertions above. All red_proof values start NONE.

The dispatch enumeration in src/server.c is checked by an inventory test: info, aas,
time_model, top_events, top_sessions, top_queries, heatmap, session_timeline, executions,
execution_detail, span_detail, exec_scatter, transitions, fingerprints, concurrency,
lock_chains, variants, interference and control. Only top_events/top_sessions/top_queries
have whole-command parity-suite citations; other cases retain NO-TEST, including commands
whose raw-only/control scope lacks the requested tested disclosure. L086 is narrower heatmap proof.

Line counts (TSV obligations, excluding runtime issue lines):

| Area | Lines |
|---|---:|
| Overview | 6 |
| AAS | 5 |
| Events | 5 |
| Sessions | 4 |
| Queries | 12 |
| Histogram | 3 |
| Timeline | 4 |
| Transitions | 5 |
| Matrix | 3 |
| Concurrency | 3 |
| Lock chains | 2 |
| Interference | 2 |
| Waterfall | 9 |
| Scatter | 4 |
| Compare | 2 |
| Fidelity | 11 |
| Capture | 3 |
| Controls | 6 |
| Bridge | 2 |
| CLI | 4 |
| Recording | 4 |
| Agreements | 17 |
| Platforms | 15 |
| Raw vs summary | 20 |
| Overhead | 6 |
| Workload | 1 |
| UI speed | 3 |
| Empty/error states | 12 |

Total: 173.

Negative controls executed one mutation at a time against the actual evaluator. Each test
failed with the assertion below, then the original source was restored; the complete
19-test suite passed after restoration. No tolerance or retry was added.

```text
a: mutation detected (exit 1)
AssertionError: 'GREEN' != 'NOT-RUN'

b: mutation detected (exit 1)
AssertionError: 'GREEN' != 'NOT-RUN'

c: mutation detected (exit 1)
AssertionError: 1 != 2

d: mutation detected (exit 1)
AssertionError: 'GREEN' != 'RED'

d-gap: mutation detected (exit 1)
AssertionError: 'test gap' not found in 'open tag-blocker #42; no annotation'

e: mutation detected (exit 1)
AssertionError: 0 == 0 : {

f: mutation detected (exit 1)
AssertionError: 0 == 0 : {
AssertionError: 0 == 0 : {
AssertionError: 0 == 0 : {
AssertionError: 0 == 0 : {

g: mutation detected (exit 1)
AssertionError: 0 == 0 : {

h: mutation detected (exit 1)
AssertionError: 'GREEN' != 'GREEN-UNPROVEN'
```

Live read-only evaluation used origin/master 42727667d2ea075b4ad8c8f5c5f3dea16d0756b3,
CI run 42861717839: GREEN 0 / total 193; GREEN-UNPROVEN 84; RED 20 (unmapped blockers);
NOT-RUN 19; NO-TEST 70; RETIRED 0. The Markdown and JSON snapshot are in the ignored
`tests/results/tag_checklist-v1/` directory. Nothing was written to GitHub.

Repository report-audit invocation was attempted on a progress draft. It returned
`AUDIT INCONCLUSIVE (auditor exit 1)` because its nested Codex client could not initialize:
`failed to initialize in-process app-server client: Operation not permitted (os error 1)`.
No SEND verdict was obtained; this is an unverified reporting-audit limitation, not a
passing gate. No workaround changed the audit tool.

The baseline regression test also covers rows introduced at the current merge base: always
read that base, without substituting its parent. The master workflow passes its parent
explicitly with --base-ref, since the checked-out HEAD is already the merged commit.
The first full make check invocation stopped before suites because ambient Homebrew
Python 3.14 lacked Playwright. The existing Python 3.11.7 installation has Playwright,
websockets, Pillow and numpy; the final full invocation selects it through PATH.
Git object writes for tree-hash/stamping are redirected to /private/tmp with the clone's
object directory as an alternate, leaving .git untouched. An initial hash preflight
failed because its alternate path resolved to the redirected directory; capturing the
clone's object path before setting the redirect corrected that invocation.

Final full-gate outcome: `make check` exited 2 (`CHECK FAILED`), with no
`.pgwt-check.stamp`. The new checklist module passed all 20 tests within that run.
The full log is `tests/results/tag_checklist-v1/make-check-final.log` (ignored).
Chromium's UI suites failed at launch with `bootstrap_check_in ... Permission denied
(1100)`; process-inspection-dependent served-asset controls reported `ps failed
(rc=126)` and 32/35 passing. Clone-based Codex-wrapper and rehearsal-provenance
fixtures also failed under the redirected Git-object environment (`does not appear
to be a git repository`). These failures were not retried, skipped or relaxed.
Both manual registration guards passed. The full-check gate remains FAIL, not a
passing stamp; hosted execution of the new workflow remains unverified (no push).
