# gate-integrity evidence (#431, #433, .gitignore evidence-class fix)

## make check
Full `make check` passed on the final tree: stamp `40215eed717764a396cb940deb87087662219688`.

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

## #431 -- check-test-registration.sh fails closed under `sh`/dash
- `431-green.log` -- `tests/test_check_test_registration.sh` full run, fixed
  script: 23/23 passed (14 original cases + 9 new #431 cases).
- `431-red-reverted-script.log` -- the SAME self-test run against the
  unmodified (pre-fix) `scripts/check-test-registration.sh`: 20/23 passed.
  The 3 failures are exactly the bug: `sh scripts/check-test-registration.sh`
  against a tree with a genuinely-removed registration printed
  `OK: test-registration guard ... — append-only invariant holds` and
  exited 0 (reproduced directly, independent of this self-test, on this
  Mac's real `/bin/sh` and a real `/bin/dash`, with a real entry
  (`test_interval_index`) deleted from `tests/unit_tests.list` -- see the
  session transcript / report for the raw `sh`/`dash`/`bash` output).

## #433 -- reconcile tests/run_all.sh vs tests/ci_smoke.sh membership
- `433-green.log` -- `tests/test_check_runner_registration.sh`: 16/16 passed
  (reconciled-GREEN, instance-3-shape comment-only RED, dropped-entry RED,
  moved-entry RED, documented-exemption GREEN, unparseable-runner RED,
  missing-runner RED, malformed-exemption RED).
- `433-real-repo-finding.log` -- `scripts/check-runner-registration.sh` run
  against THIS real repository (no repo-dir argument): correctly finds the
  two real, pre-existing gaps the issue names in spirit --
  `test_capture_smoke.py` and `test_uprobe_fired.py` are invoked only by
  `tests/ci_smoke.sh`, not `tests/run_all.sh`. Left unfixed and
  undocumented-as-exempted deliberately: fixing either means editing
  `tests/run_all.sh` (a live-tier test-assertion change), out of scope for
  this guard-building branch per the brief ("Do not touch ... any live-tier
  test assertion").
- Bypass-suite demonstrations (not saved as log files; done interactively
  and reported in the final report): disabling the guard's comment-strip
  step reproduces a false GREEN on the instance-3-shaped fixture (14/16);
  disabling the "mentions python3/bash but extracted nothing" parser
  self-check reproduces a false GREEN on the unparseable-ci_smoke.sh
  fixture (14/16). Both restored before this commit.

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
