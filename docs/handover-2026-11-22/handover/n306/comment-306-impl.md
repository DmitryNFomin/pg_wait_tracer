**306-a implementation: first round measured on both systems (2026-11-21); not merged yet**

Branch `agent/306-sampler-nice`, commit 8cc67d0f (tree 1c1784b2): sampler at nice -10 by default; a startup check of the tracer's and PostgreSQL's CPU groups with a warning when they differ; `test_cross_validate_tiered` starts the tracer as a plain transient system unit with pgbench outside and reads back, mid-run, the three CPU groups and the sampler's nice value (a mismatch is exit 80, INVALID). The 8.8 pp bound and the known-failing list are unchanged.

Deviation in percentage points, min / median / max, n=5 per cell, every run exit 0:

| System | Launched from | 10 Hz | 100 Hz |
|---|---|---|---|
| Ubuntu 24.04, kernel 6.8, cgroup v2 (dev VM) | login session | 3.42 / 4.01 / 4.39 | 2.51 / 3.31 / 3.99 |
| | system service (the CI runner's shape) | 1.88 / 3.59 / 5.44 | 2.24 / 3.27 / 3.61 |
| Rocky 8.10, kernel 4.18, cgroup v1 (dev VM, clean boot) | login session | 2.26 / 2.86 / 3.81 | 2.74 / 3.61 / 3.67 |
| | system service | 2.45 / 3.55 / 3.86 | 2.91 / 3.18 / 3.63 |

- Layout read back in every one of these runs: tracer and backend in the same CPU group, pgbench in another, sampler nice -10. Ten further startup-only checks per context per system: same layout, exit 0.
- For comparison, today's master in the CI runner's shape on the Ubuntu gate box reads 7.6 to 9.3 at 10 Hz and 8.2 to 8.7 at 100 Hz (issue 675, measurements 3 and 4).

What the first EL8 campaign showed, and what the clean boot corrected:
- In the implementer's own EL8 campaign every standalone run exited 80: the tracer's unit was in cpu cgroup `/system.slice` while the long-running postmaster was in `/`. Each such split followed a transient unit with a CPU property that the campaign itself had started for its controls.
- On a clean boot (no unit with a CPU property): 20 of 20 startup checks and 10 of 10 runs had both in `/`.
- After one `CPUShares=512` unit was started and stopped: one reading 2 s later had the postmaster in `/system.slice/postgresql-18.service` and a new plain unit in `/system.slice`; the next reading, and all later ones, had both in `/` again without any action. Not re-measured; the duration of that window is unknown.
- Read-only probes: on the EL8 gate box all postmasters and a plain unit are in cpu `/`; on Ubuntu gate box 2 `system.slice` has no cpu controller enabled. Gate box 1 was busy and not probed.

Independent code review: no code blocker. Three documentation corrections (the changelog entry still describes the old behaviour; two passages call the test's deviation a "CPU under-read"; the separate-group explanation covers cgroup v2 only) and one cleanup nit go into a short second round, together with the two EL8 controls that the first campaign left confounded.

Still open after this lands: the residual of about 2 to 4 pp, this issue's original question. The scoped known-failing entry for the EL8 form (issue 675) is not touched in this change; it is reviewed against the EL8 gate box's own results after the merge.

Raw: lead scratchpad `n306/` and `/private/tmp/306a-evidence/` on the lead's Mac.

Refs #306
