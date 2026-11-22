Implements the owner's decision of 2026-11-19 on issue 306 ("306 - a"). The residual under-read that issue 306 is about stays open; this PR does not settle that issue.

## What changes

- **Default.** The sampler runs at nice -10 unless `PGWT_SAMPLER_NICE` says otherwise (an explicit value is applied as given, so `0` turns the default off; an invalid value warns and the default applies; a value that cannot be applied warns and the daemon continues).
- **Startup check.** The daemon compares its own CPU control group with PostgreSQL's (cgroup v2: the nearest group with the cpu controller enabled; v1: the cpu hierarchy path) and warns once when they differ, because the nice value does not act across groups. Unknown is reported as unknown. Never an exit.
- **Documentation.** README, INSTALL and the changelog state the default, where the sampled tier's accuracy figures apply (tracer and PostgreSQL in one CPU group), that no figure is claimed where they differ, and how such a split arises on cgroup v1 and v2.
- **The cross-validation test runs in the deployment layout.** `test_cross_validate_tiered` starts the tracer as a plain transient system unit and pgbench outside its CPU group, whatever context the test is started from, and reads back mid-run the three CPU groups and the sampler's nice value. A mismatch is exit 80 (INVALID), never a pass and never the known-failing exit. The unit's lifetime is bounded, so a killed test cannot leave a tracer running. The 8.8 pp bound, `tests/xval_tolerance.sh` and the known-failing list are unchanged.

## Why

On both Ubuntu gate boxes the required box tier failed on this test in 6 of 42 CI jobs over two days: started as a default system service, as the CI runner starts it, the sampled tier read about 7.6 to 9.3 pp under the exact tier, against a bound of 8.8. Measured on 19 November and again on 21 November (issue 306, issue 675).

## Measured with this change (percentage points, min / median / max, n=5 per cell, every run exit 0)

| System | Launched from | 10 Hz | 100 Hz |
|---|---|---|---|
| Ubuntu 24.04, kernel 6.8, cgroup v2 | login session | 3.42 / 4.01 / 4.39 | 2.51 / 3.31 / 3.99 |
| | system service (the CI runner's shape) | 1.88 / 3.59 / 5.44 | 2.24 / 3.27 / 3.61 |
| Rocky 8.10, kernel 4.18, cgroup v1, clean boot | login session | 2.26 / 2.86 / 3.81 | 2.74 / 3.61 / 3.67 |
| | system service | 2.45 / 3.55 / 3.86 | 2.91 / 3.18 / 3.63 |

Twenty startup-only checks per system read the intended layout every time. Controls, each shown in isolation on both systems: the default removed (exit 80 on the nice value), a CPU property on the tracer's unit (exit 80 on the layout, the daemon's warning printed), pgbench left in the tracer's group (exit 80). On EL8, after a unit with a CPU property stops, a new unit and the running postmaster are in different cpu cgroups for about one to two seconds; a run started in that window is INVALID by design.

## Gates on this tree (commit 85af1a81, tree 3ee3af64)

- `make check` (full): `CHECK PASSED (full) — stamp 3ee3af64569a499707e9dc3a0b82f61008c9da1c`.
- Ubuntu box-check: `box-check: commit=85af1a81… tree-hash=3ee3af64… built-from=3ee3af64…`; `Executed: 238 (passed 237, failed 0, refused 0, known-failing 1 (1 xpass)), skipped 5, excluded 1, total 244`; `Live UI smoke: overall=PASS`; this test: 10 Hz 1.27, 50 Hz 3.06, 100 Hz 3.60, 200 Hz 2.95. The unexpected pass is `test_init_hint_recovery` (issue 959), as on master.
- EL8 box-check: same commit and tree; `Executed: 238 (passed 232, failed 0, refused 5, known-failing 1 (1 xpass)), skipped 5, excluded 1, total 244`; `Live UI smoke: overall=REFUSED (refused: waterfall, scatter)`, the EL8 phase-contract state as on master; this test passed while still listed as known-failing (issue 675): 10 Hz 4.44, 50 Hz 2.85, 100 Hz 3.46, 200 Hz 2.77.
- Both box checks ran on development VMs built from the gate snapshots, not on the CI runners.

## Review

Independent code review of the implementation commit: READY-with-nits, no code blocker (the CPU-group decision tried against 30 further fixture cases, including under sanitizers; the nice value goes on the sampling thread; no exit or retry path; an invalid layout can never end as a pass). Its three documentation corrections and one cleanup nit are the second commit, which changes documentation and four lines of the test.

## Follow-ups, not in this PR

- The EL8 known-failing entry for this test is reviewed against the EL8 gate box's own results after the merge; while it stays, the EL8 gate tolerates up to 15 pp.
- The 8.8 pp bound is re-derived from the shipped configuration, pre-registered before its confirmation runs.
- The startup warning is on the daemon's output and in the documentation; showing it in the status interface waits for the branch that holds those files.

Refs #306
Refs #675

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01Dt8SuoJQ8W12sKXzVgE8c2
