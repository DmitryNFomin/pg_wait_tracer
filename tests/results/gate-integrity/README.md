# gate-integrity evidence (#431, #433, .gitignore evidence-class fix)

## make check
Full `make check` passed on the final tree (after the orphan-scan round):
stamp matches `scripts/tree-hash.sh`'s output on the committed tree (see the
commit message / final report for the exact value -- verify with
`cat .pgwt-check.stamp` vs `scripts/tree-hash.sh` on this tree, which must be
equal).

## make box-check
Ran on the persistent gate box (`root@2.28.45.47`, idle at load 0.10, lock
free at start) per CLAUDE.md's "persistent boxes first" rule -- no ephemeral
VM created.

```
box-check: commit=97dca0c209e67f0d1160a4221167f45fb09a964e tree-hash=b2fefd85ad2a71afc48e52b02a0edae1b753df17
box-check: target=root@2.28.45.47 OS=ubuntu PG=18 EPHEMERAL=0
box-check (ubuntu, PG=18, EPHEMERAL=0) exit=0 — summary:
  Executed: 135 (passed 135, failed 0, known-failing 0 (0 xpass)), skipped 4, excluded 0, total 139
  Mode:     --require-live (live-section skips counted as failures)
  Live UI smoke: overall=PASS
```
Full log: `tests/results/box-check-gate-integrity.log`.

**Provenance note**: this box-check's rsync captured the tree at tree-hash
`b2fefd85ad2a71afc48e52b02a0edae1b753df17` -- the state right after the first
passing `make check` for #431+#433, started deliberately BEFORE the later
Mac/docs-only additions below (made while the box-check ran, per CLAUDE.md's
"start box-check first" rule). None of those later additions touch anything
`make box-check` exercises (it runs `tests/run_all.sh` on Linux; it never
reads `scripts/check.sh`, `.gitignore`, or `CLAUDE.md`), so this run remains
valid live-tier evidence for the final tree:
  - `.gitignore` (tests/results/ evidence-class ignore rule)
  - `tests/test_results_evidence_ignore.sh` (new, Mac-side only, wired into
    `scripts/check.sh`, never into `tests/run_all.sh`)
  - `scripts/check.sh` (wires both new self-tests into `make check`)
  - `CLAUDE.md` (documents the new guard alongside its two siblings)
  - the orphan-scan round (`scripts/check-runner-registration.py`'s orphan
    scan, `Makefile` read-only addition as a 4th registration source,
    `tests/test_check_runner_registration.sh`'s 8 new cases) -- same
    Mac-side-only property: `make box-check` runs `tests/run_all.sh` on
    Linux and never reads `scripts/check-runner-registration.py` or this
    self-test at all, so none of this round's changes touch anything the
    box exercises either.

## #431 -- check-test-registration.sh fails closed under `sh`/dash, and when `sort` fails
- `431-green.log` -- `tests/test_check_test_registration.sh` full run, fixed
  script: 25/25 passed (14 original + 9 `sh`/dash cases + 2 new
  failing-`sort`-on-PATH cases, adviser round 2).
- `431-red-reverted-script.log` -- the SAME self-test run against the
  unmodified (pre-fix) `scripts/check-test-registration.sh`: 20/23 passed
  (pre-dates the sort-shim case). The 3 failures are exactly the bug: `sh
  scripts/check-test-registration.sh` against a tree with a
  genuinely-removed registration printed
  `OK: test-registration guard ... — append-only invariant holds` and
  exited 0 (reproduced directly, independent of this self-test, on this
  Mac's real `/bin/sh` and a real `/bin/dash`, with a real entry
  (`test_interval_index`) deleted from `tests/unit_tests.list` -- see the
  session transcript / report for the raw `sh`/`dash`/`bash` output).
- **Adviser round 2 finding, fixed**: the guard checked `comm`'s own exit
  status but not the two process substitutions feeding it. A failing `sort`
  (missing, or present but exiting non-zero with no output) made both
  substitutions present an empty-but-valid stream to `comm`, which then
  succeeded and reported "nothing missing" -- the identical root cause as
  the `sh`/dash bug, one step further upstream. Fixed by sorting each side
  as a plain pipeline first (so `set -o pipefail`, already active, gives
  `sort`'s own exit status), checking each, THEN feeding the two
  already-validated strings to `comm` via `printf | <(...)` (printf of a
  literal string cannot fail). RED demonstrated interactively: a shimmed
  `sort` on PATH that exits 1 with no output, run against a tree with a
  genuinely-removed registration, printed OK and exited 0 pre-fix; the new
  self-test case (shimming `sort` the same way) reproduces this red against
  the reverted script (23/25) and passes green against the fix (25/25).
  Permanent negative control, not a one-off repro, per instruction.

## #433 -- reconcile tests/run_all.sh vs tests/ci_smoke.sh membership, plus an orphan scan
- `433-green.log` -- `tests/test_check_runner_registration.sh`: 24/24 passed
  (16 reconciliation cases + 8 orphan-scan cases: orphan-file RED, same file
  registered-into-check.sh GREEN, `.test.mjs` glob-discovery does-not-trip
  GREEN, Makefile-only-registration GREEN, missing-`scripts/check.sh` RED).
- `433-real-repo-finding.log` -- `scripts/check-runner-registration.sh` run
  against THIS real repository (no repo-dir argument): correctly finds
  - the two real, pre-existing RECONCILIATION gaps the issue names in
    spirit -- `test_capture_smoke.py` and `test_uprobe_fired.py` are
    invoked only by `tests/ci_smoke.sh`, not `tests/run_all.sh`;
  - four real, pre-existing ORPHANS (invoked by NO runner): and
    `test_check_test_registration.sh` (#431's own self-test -- its header
    says "run it by hand", same as `check-test-registration.sh`'s own live
    invocation, but the orphan scan does not distinguish "deliberately
    manual" from "forgotten" -- that judgment is the exemption list's job,
    left to the issue owner per instruction), `test_gesture_gate.py` and
    `test_startup_probe_degrade.py` (both have a "Usage: ..." docstring
    describing a manual, env-var/root-gated invocation -- likely
    deliberate manual tools, not bugs, but not mine to judge), and
    `test_v9_open_cpu_wired.sh` (already extensively documented in both
    `run_all.sh` and `ci_smoke.sh` as deliberately unregistered -- the
    strongest exemption-list candidate of the four, left unexempted
    anyway per instruction).
  All six names left unfixed and NOT added to
  `tests/runner_registration_exemptions.list`, per instruction ("do not fix
  them and do not exempt them -- list them in your report").
  One FALSE POSITIVE was found and fixed, not reported as a finding:
  `test_check_lock.sh` is invoked by the root `Makefile`'s `check:` target
  (`@bash tests/test_check_lock.sh`), a 4th registration path this guard did
  not originally scan -- added as a required runner (`extract_bare_tests_path_registrations`
  reused against `Makefile` the same way as `scripts/check.sh`) so this
  specific false positive cannot recur; the self-test's Case 12 pins it.
- Bypass-suite demonstrations (not saved as log files; done interactively
  and reported in the final report): disabling the guard's comment-strip
  step reproduces a false GREEN on the instance-3-shaped fixture (14/16);
  disabling the "mentions python3/bash but extracted nothing" parser
  self-check reproduces a false GREEN on the unparseable-ci_smoke.sh
  fixture (14/16); disabling the orphan-scan computation entirely (`orphans
  = []`) reproduces a false GREEN on the orphan-fixture case (22/24, the
  orphan-fixture case and its "names the file" sub-assertion both flip to
  FAIL). All three restored before this commit.
- **Adviser round 2 finding, fixed**: `extract_bare_tests_path_registrations`
  (used for `scripts/check.sh` and the root `Makefile`) matched any
  `tests/<file>` PATH TOKEN present on a non-full-line-comment line,
  regardless of what came before it -- so a bare mention
  (`echo "tests/test_orphan.py is disabled"`) or a filename inside a
  TRAILING inline comment (`run bash tests/x.sh  # tests/evil.sh not run`)
  both registered the named file as if it had actually been invoked. Fixed
  by requiring the token immediately before the path to be one of
  `bash`/`python3`/`run`/`--test` (the real invoking keywords every actual
  registration in this codebase uses), and by stripping a trailing
  `  # ...` comment before tokenising a line at all. 2 new permanent self-test
  cases (28/28 total): a bare-mention fixture and a trailing-inline-comment
  fixture, both asserting the named file is reported as an orphan, not
  silently registered. RED demonstrated by reverting to the pre-fix
  extractor: both new cases (4 assertions) failed with a false `OK`
  (24/28); restored before this commit.

## .gitignore -- tests/results/ evidence directories ignored as a class
- `gitignore-green.log` -- `tests/test_results_evidence_ignore.sh`: 5/5
  passed against the fixed `.gitignore`.
- RED demonstrated interactively (not saved as a log; see report): with the
  pre-fix `.gitignore` restored via `git stash`, the same self-test scored
  4/5 (the brand-new-directory-name control failed), and
  `scripts/tree-hash.sh` measurably moved
  (`b2fefd85...` -> `245c50d8...`) when a brand-new
  `tests/results/<name>/` directory was created -- exactly the drift this
  fixes. With the fix restored, the same experiment leaves the hash
  unchanged.
- `git ls-files -i -c --exclude-standard` prints nothing (no tracked file
  became ignored) -- verified against the final tree.
