# Tag checklist v1

`tests/tag_checklist.tsv` is one guarantee per stable ID. Never delete/reuse IDs: the
evaluator unions every TSV version from `git log --first-parent --format=%H <sha> --
tests/tag_checklist.tsv`, including the evaluated commit. Missing/unreadable history and
shallow clones refuse; checkout uses `fetch-depth: 0`.

Retire explicitly with `state=retired` and a nonempty `reason` column; RETIRED remains
printed and cannot count. A historical retirement cannot become active again.

Selectors are CSV `file::name` entries within TSV; names identify callables, exact Node
titles or the whole named shell/Python suite. Evidence is its shared
`workflow-file/job-display-name/exact-step-name`, or `box-check`; differing steps
require separate rows.

GREEN requires successful steps for exactly the requested SHA, verified red-proof
identity and no mapped open blocker. GREEN-UNPROVEN has successful steps but
`red_proof=NONE`; only GREEN counts toward the tag.

WEAK is the sixth active state: substring, presence (exists/nonempty/positive), ordering
and exit-code checks cannot certify a correct number. V1 conservatively makes every
successful row with one of those tolerances WEAK, including nonnumeric claims and rows
with a red proof. Other tolerances are `exact` (equality on the value itself) or the
test's numeric bound.

RED means a failed step or an open `tag-blocker` with `line:Lnnn`; a blocker over
successful evidence is annotated `test gap`. Every unmapped blocker adds its own RED
line; an unknown line label refuses the report.

NOT-RUN means missing/skipped/cancelled/unfinished evidence or box-only evidence.
NO-TEST means no test or no evidence; its tolerance must be empty. Unsupported
remainders stay as new NO-TEST rows. Historical versions retain their old tolerance
cells for append-only validation. RED overrides WEAK; absent evidence stays NOT-RUN
before WEAK is considered.

Run `scripts/tag-checklist.sh <full-sha>` for Markdown, `--json` for JSON, and
`--markdown-output <path>` for both from the same snapshot. Markdown identifies the SHA,
CI run ID, attempt, status and conclusion. A SHA without a CI run refuses; valid
non-GREEN reports exit 0, not tag-ready.

Re-run rule: use the highest `databaseId` among master-push runs for this SHA, even if
an older run passed. `gh run view` reads that run's latest attempt; a later passing
attempt supersedes its failure. `--run-id N` overrides run selection (as in
workflow_run), still reading its latest attempt and enforcing SHA identity. In-progress
runs never certify early successful steps.

`red_proof` accepts NONE, PR #N/#N/PR URL or a full commit SHA/commit URL. Every cited
PR must exist and be MERGED with mergedAt, and its changed files (`gh pr view --json
number,state,mergedAt,files`) must include a test file from that row. Every cited commit
must satisfy `git merge-base --is-ancestor <proof> <evaluated-sha>` and touch a test
file from that row (relative to its first parent for a merge); file paths are the parts
before `::` in its CSV selectors. Unavailable, invalid or unrelated proof data refuses
the report with the row ID, proof and test files. This does not prove that the test was
red for the right reason. The reviewer of a PR filling a `red_proof` must paste the red
output.

`make check` runs the fixtures AND `scripts/tag-checklist.py "$(git rev-parse HEAD)"
--check-history` on the real TSV without querying CI; it also asserts
`.github/workflows/ci.yml` still has `name: CI`, on which workflow_run depends, and the
checklist workflow grants exactly `contents`, `actions`, `issues` and `pull-requests`
with `read` access.

Offline inputs: `history.json` is an array of historical TSV strings; `proofs.json` maps
proof references to PR metadata (including `files: [{"path": "tests/example.py"}]`) or
commit observations `{ancestor,descendant,isAncestor,files}` (with `files` an array of
changed path strings); `run-list.json`, `run-<id>.json` and `issues.json` mirror gh
observations. Wrong SHA, void jobs, malformed input, deleted IDs and Git/gh failures
exit 2 without a partial readiness table.

V1 sees steps, not per-test terminal results/internal skips/XFAILs; native matrix/box
evidence remains uncovered or NOT-RUN. Containers do not attest EL8 4.18. Blocker
changes need a fresh invocation. Deferred: SHA/platform/test records and scheduled
real-box runs. The workflow only uploads artifacts; it never edits issues or labels.

Scope corrections: re-read all 174 original physical TSV lines (header + 173
obligations, L001–L173). Round 2 changes 82 existing rows: 68 statement corrections, 49
split source rows preserving remainders in L174–L215 and L263–L269, 7 re-citations
(L027, L037, L059, L072, L098, L099, L150), and 35 test-bearing rows with WEAK
tolerances. Categories overlap; no original ID is removed. L216–L217 add
Transitions-vs-Overview/AAS (#808) and Compare recovery; L218–L231 preserve
README/INSTALL guarantees; L232–L262 add one failure-injection obligation per relied-on
gate, including pacing residual #832. These additions have no tests, rather than
assuming a passing gate proves it detects its own failure.

- L001/L013/L015/L021 are canned displays, not production numerical oracles.
  L013/L015/L021 use substrings, not field equality; L008 total text proves no
  percentages, L014 em-dash inclusion does not exclude fabricated values, and L024
  overflow text does not prove a capped numeric value. Their stronger guarantees remain
  L174/L176–L178/L182/L184. L010 tests a class click and a double-click that zooms
  WITHOUT drilling; event identity remains L175.

- L002 duration/%DB bounds are 0.1%/1% relative, with only CPU/IO percentages asserted
  (others L263); L003 is 1% components and 2% off-CPU/partition; L006 is 5% relative
  with 0.001 AAS at zero, checking only the first bucket (all buckets L264). L011 omits
  CPU Max (L265), while L012 quantile values cover Lock/LWLock only (other events L266).
  L005/L083 separate IO-worker/CPU-sample cases (2% versus 5%/6%). L012/L084 separate
  server percentiles (1%) from browser substring rendering; complete browser cells
  remain L202. L016 retains its numeric oracle but not conditional first-two-row ranking
  (L179); L025 P95 is within 1e-9 ms, not exact.

- L017/L026 preserve uncovered Sessions percentages and end-to-end query latency
  populations; L019 hover/profile markers do not prove full SQL/segment values (L180).
  L020 has class, not event-ID filters (L181); L023 does not establish every planning
  count/average (L183). L027 now cites the actual state-axis assertions; full
  elapsed/state rendering remains L185. L021/L085 distinguish percentage text from
  empty-state unattributed-time disclosure.

- L029/L086 separate histogram builder from aligned-grid C parity; tooltip numeric
  equality remains L186. L030 cell drill pivots to Queries, not Histogram. L031/L087
  separate exact server timestamps from first-bar mock geometry (<1 s raw-duration
  error); all-bar geometry remains L211. L034 counts only four selected links and one
  edge duration (remaining edges L267); L035 retains restart/idle-classification checks.
  L037 now includes section1_truncation/section2_boundaries: execution P95 is over
  capped written samples, while planning P95 remains L187; planning/execution step
  averages are tested. L038 signature substrings do not prove complete fingerprints
  (L188).

- L040/L088, L041/L089, L046/L092, L048/L093, L049/L094 and L051/L095 separate different
  evidence steps; L051 checks retained/excluded counts, not the retained point identity
  in the null/zero fixture (L268). L050 tests kept_count metadata, not the actual
  returned budget (L213). L039/L041 have numeric model checks but tooltip/burst HTML
  only substring checks (stronger values L189–L190); L088 tests cap-note wording, not
  complete graph semantics (L212). L089 is only >=9 occupied buckets, not an exact
  burst/peak oracle. L046 proves server sort fallback; drawable selection is separately
  L092. L045 complete row PID/QID/start truth and L048 whole uncut wait truth remain
  L191–L192; L093 readout text is substring-only (L203). L094 is >0 px margin at
  1710x981, >=16 px at 1710x1000; L096 endpoints allow 5 ms.

- L043/L090 distinguish inferred fixture candidates from actual PostgreSQL lock
  identity; L044/L091 distinguish positive overlap/score from independently correct
  values. L052 pins ranks, noise floor and new/gone/null ratios, not finite ratios or
  every absolute delta (L193). L054/L096 split AAS server ranges from mock banding;
  L055/L097 cover only their named APIs/panels (remaining server/UI contracts
  L194/L204).

- L056 proves tier change/button text/Stop presence, not grant/budget/expiry/denial or a
  working Stop (L195). L057 asserts model tier/rates/drops/fire count/budget, not
  faults, near/dropped anomalies or baseline AAS (L196). L058/L101 distinguish low/high
  estimator predicates from measured accuracy. L059/L072/L098/L099/L150 now name their
  whole assertion-bearing shell suites rather than setup/transport helpers; L098 budget
  predicates are not independent counter accuracy, and L099 proves int/float predicates
  and growth, which still admit bool/non-finite fields (finite numeric types L269). L100
  preserves that missing oracle.

- L060 sorting checks only an indicator (actual row order L197); L061 time controls have
  partial checks (complete preset/zoom correctness L198). L063 reconnection is status
  text only (actual restored data flow L214); it also tests degraded transport and
  Retry, not all-view recovery; L064 is canvas identity through a tick, with chaos
  survival separately L102. L065 timestamp substrings do not prove complete numeric
  fields (L199); L066 server_version is presence-only although protocol is compared to
  the client revision.

- L067 and L103 check exit codes only: real target/view output remains L200;
  multi-window interval/field truth remains L205. L068 proves headers, allowed state
  tokens and order, not independent live state/numeric truth (L201). L069 literal
  comparisons cover selected time/event fields, not occurrence counts (L209); L070
  counts bounded records and checks unbounded time-model totals, not clipped straddling
  durations (L210). L070/L104 distinguish C replay from fidelity substring disclosure.

- L074 covers query-filtered raw-route agreement (relative error 1e-7, spread <=0.001
  ms), not general raw/summary equality. L075 keeps uncompromised parity uncovered; L148
  diagnoses #410 pinned differences. L121–L123 compare scalar fields between
  summary-present/removed fixture directories (queries: first eight); the route oracle
  is time_model only, so complete actual-route/nested/population equality remains
  L206–L208. Numeric comparison uses max(1e-6 absolute, 1e-9 relative).

- L078 states +/-8.8 pp with disclosed bias; L079 permits a different-window 0.3–3.0
  DB-Time ratio, not equality (L145–L147). L080 does not establish an EL8 UI/docs
  contract; L081/L106–L117 retain the native matrix, and PG16 fatal-refusal CI cannot
  prove never-fails. L082 proves synthetic keys only; L105 keeps missing PG13
  graceful-startup coverage.

- L149 uses final paired-median loss <15%, with the existing confirmation policy; L150
  bounds only reported TPS loss at <50%, without asserting tracer liveness/full capture
  (L215). Neither is an owner-aligned release budget: L151–L154 keep those unbudgeted
  mode obligations. L155 keeps realistic workload, L156–L158 separate 3000 ms live first
  paint, unbudgeted interactions and 10 s replay API latency. L159–L170 and L217 keep
  per-view recovery uncovered; L171–L173 keep end-to-end execution
  duration/timestamp/P95 oracles uncovered.
