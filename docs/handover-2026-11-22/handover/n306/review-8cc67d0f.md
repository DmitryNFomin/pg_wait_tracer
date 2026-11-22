# Review of 8cc67d0f (agent/306-sampler-nice, parent 7140a517)

Reviewer: Claude (independent; not the author). Scratch worktree: SP/rv16/wt (detached at 8cc67d0f). I edited nothing outside scratchpad/rv16/, committed nothing and pushed nothing.

## Verdict: READY-with-nits

I found no code blockers. The product code (src/cpu_group.c, src/sampler.c, src/daemon.c) and the test's layout guard are correct. When the evidence is missing or bad, they refuse or warn. They never answer "same" without evidence, and no INVALID layout can end as PASS or as exit 79.

Three documentation items are should-fix. Each is a small doc-only edit:
- CHANGELOG [Unreleased] now contradicts the shipped behaviour.
- README calls a "deviation" a "CPU-share under-read".
- The docs say nothing about the cgroup v1 placement effect that this round's own data shows.

The EL8 evidence gap is real. On EL8, 5a and 5c were not shown in isolation, and the qualification has 0 valid draws. The PR body must say so. It is not a code defect.

## Evidence identity
- VERIFIED: `git rev-parse 8cc67d0f^{tree}` printed `1c1784b2a598bd6e002b11dd83d908b15e2551f2`. That is the tree hash in both machines' final-build.log (`box-check: commit=7140a517… tree-hash=1c1784b2… built-from=1c1784b2…`). So the remote evidence was produced from exactly the committed tree.
- VERIFIED: `git diff --check 7140a517 8cc67d0f` exit 0. `git diff --name-only` on contracts, src/server.c, src/core, web, scripts/box-tier.sh, .github, tests/xval_tolerance.sh and tests/run_all.sh is empty.
- VERIFIED: tiered_classify, tiered_arm_assertions and tiered_workload_validity are byte-identical to 7140a517 (awk-extracted and diffed: "identical" ×3). TOLERANCE=8.8 is unchanged.
- VERIFIED: the 14 comment ids on issue 306 (fetched by gh api just now) are the same 14 the brief quotes. No comment is newer than 7065660887.
- NOT CHECKED: `.pgwt-check.stamp` against `scripts/tree-hash.sh`. Per my brief, no `make check` was run. There is no stamp in the scratch worktree and no `make box-check` log under tests/results for this commit. The Linux evidence is the implementer's box-build.sh + run_all on two VMs.

## Should-fix

1. CHANGELOG.md:52-56 (not touched by the commit). The [Unreleased] entry still says the following:
   - `unset/empty leaves it unchanged`
   - `0 verifies without changing it`
   - `Nice does not remove the sampled CPU figure's lower-bound bias`

   VERIFIED by reading the lines: line 11 is `## [Unreleased]` and the entry is above `## [0.13]` (line 475).
   - Scenario: a user reads the next release's notes and leaves PGWT_SAMPLER_NICE unset.
   - Expected: notes say the sampler runs at nice -10.
   - Actual: notes say niceness is left unchanged and that nice does not help.

   All three statements are false at 8cc67d0f (src/sampler.c:78-82 sets -10; src/sampler.c:93 now calls setpriority for 0). This is the "anything still says nice does not help" case from the review brief. The commit adds no CHANGELOG entry.

2. README.md:1634 and README.md:1930-1933 relabel the quantity. Measurement 7 (comment 7004963488) reports "Median deviation: Ubuntu 7.45 pp, nice 4.15 pp; EL8 9.85 pp, nice 3.55 pp" and "EL8 read 10.35 pp". The README prints these as "median CPU-share under-read fell from 7.45 to 4.15 pp…" and "the EL8 arm read 10.35 pp low".
   - The test's deviation is the maximum |exact−sampled| over events with an exact share of at least 2%. It is unsigned, and it is not always CPU. The README text this commit deletes said so itself: "sometimes CPU and sometimes transactionid".
   - Measurement 4b reports the two quantities separately (C-nice deviation median 3.75 vs signed CPU error +3.52), so they are not interchangeable.
   - Scenario: a reader takes 4.15 pp as the signed CPU under-read in the shared-group service setup. The source only supports "max per-event share deviation, median 4.15 pp".
   - Read from source (comment text vs README lines), not reproduced. Fix: say "median max-share deviation" or similar.

3. INSTALL.md:521-524 and README.md:1647-1649 tie the separation only to cgroup v2 ("on cgroup v2, a CPU property on any unit under system.slice can …"). Nothing tells an EL8 (cgroup v1) user that the same thing happens there, and that it persists after the property unit is gone.
   - This round's raw logs show it twice (see Evidence section, EL8 chronology). Comment 7001732359 (measurement 5) already records the v1 "/" vs "/system.slice" variation, so the docs can cite an issue comment without using this round's numbers.
   - Scenario: an EL8 user once ran any system.slice unit with CPUShares, then starts the tracer as a plain unit.
   - Expected: the docs explain the startup warning.
   - Actual: the warning "sampler CPU groups differ: tracer v1:/system.slice, PostgreSQL v1:/" fires, and the docs offer only the v2 explanation.

## Nits

4. tests/test_cross_validate_tiered.sh:482 + :323-325 (read from source, not reproduced). The tracer now runs as a separate `--collect` system unit in `--daemon` mode, which never exits while PostgreSQL lives. Only the EXIT/INT/TERM/HUP traps stop it.
   - If the test is SIGKILLed (for example a CI runner's hard kill after its INT/TERM grace, or `kill -9`), the unit keeps tracing the gate box's PostgreSQL indefinitely, and no later run reaps `pgwt-xval-*` units.
   - At 7140a517, a process-group kill also took the child daemon down. A pid-only SIGKILL orphaned it in both versions.
   - INT, TERM and HUP are handled. The implementer demonstrated TERM cleanup on both machines (ubuntu/signal-units-after.txt empty; el8/signal-before-readback.exit = 143, .after empty).

5. Commit message: the last line is `Co-authored-by: Codex …`, not `Refs #306` as the brief asked. This is harmless ("Refs" does not close).

## What I verified as correct

### src/cpu_group.c, the rule in brief item 2
VERIFIED. `make -C tests test_cpu_group cpu_group_readback` built on the Mac and `./test_cpu_group` printed "20 CPU-group fixture cases passed", exit 0. The same test under `-fsanitize=address,undefined` also passed with no sanitizer report.

My own fixture program (SP/rv16/fx/fx.c, ASan+UBSan) ran 30 cases, with no sanitizer report:
- **v2:**
  - nested slices with cpu enabled at system.slice only: DIFFERENT (/system.slice/tracer.service vs /system.slice/system-postgresql.slice)
  - two units in the same non-cpu slice: SAME
  - daemon pid = postmaster pid: SAME
  - missing /proc entry: UNKNOWN "No such file"
  - path with a space and a `\x2d` escape: handled
  - root vs root: SAME
  - root vs a child when root enables cpu: DIFFERENT
  - no trailing newline: OK
  - CRLF: UNKNOWN
  - " (deleted)": UNKNOWN
- **v1:**
  - `cpu,cpuacct` vs `cpuacct,cpu` at the same path: SAME
  - "/" vs "/system.slice": DIFFERENT
  - named hierarchy only: UNKNOWN "no CPU hierarchy"
  - cpuset+cpuacct only: UNKNOWN
  - " (deleted)": UNKNOWN
  - two cpu lines: UNKNOWN
  - a colon inside the path: SAME
  - space in path: SAME
- **Hybrid:** the v1 cpu line wins and is labelled "v1 (hybrid: …)".
- **Sizes and malformed input:**
  - path length 4095: accepted
  - path length 4096: UNKNOWN
  - line over 16 KiB: UNKNOWN "input too long"
  - v2 path plus root over 4096: UNKNOWN "too long"
  - empty file: UNKNOWN
  - blank middle line: UNKNOWN
  - postmaster pid 0: UNKNOWN

The one case my program printed FAIL was my own wrong expectation: system.slice enabling cpu while root does not, which the kernel cannot produce. Re-derived by hand, the code's answer follows the brief's rule. No input made it crash, read out of bounds, or answer SAME without evidence. Mixed-version DIFFERENT (v2 "/" vs v1 "/") is not reachable on a real host.

### Thread placement
VERIFIED from source. setpriority(PRIO_PROCESS, 0) runs from pgwt_daemon_init on the main thread (src/daemon.c:1721). The sample_timer_fd is serviced in pgwt_daemon_run's epoll loop on that same thread (src/daemon.c:1951, called from src/pg_wait_tracer.c:763/811). The pgss resolver thread starts earlier (daemon.c:1626), so it keeps the original nice. cleanup restores once (daemon.c:2326-2336). struct pgwt_daemon is calloc'd (pg_wait_tracer.c:312), so `valid` starts at 0.

### Sampler scheduling paths
VERIFIED by reading src/sampler.c:75-130 and daemon.c:713-759.

| Input | What is applied | Message |
|---|---|---|
| unset | -10 | INFO "applied nice -10; nice -10 reduced sampled CPU under-read in measured plain service setups sharing PostgreSQL's CPU group; residual under-read remains." |
| explicit -20..19 | applied as given | same INFO line |
| explicit 0 | setpriority(0) | same INFO line |
| "" or a malformed value | -10 | one WARNING "… invalid (expected integer -20..19); applying default nice -10." |
| setpriority refused | value not applied | one WARNING "nice not verified (requested nice N, readback M): <errno>; continuing without retry; sampled CPU may under-read under CPU contention; no scheduling improvement is verified by this request." |

No path exits or retries. Each message stays within what measurements 4b, 5, 7 and 8b support. The CPU-group messages match brief item 2:
- DIFFERENT names both groups and says nice does not act across groups and the accuracy figures do not apply.
- UNKNOWN gives one WARNING with the reason.
- SAME gives one INFO line.

### Untouched consumers
VERIFIED. `git grep` at 7140a517 for "sampler scheduling", "left unchanged (default)", "measured NOT to move", LEAVE_ALONE, PGWT_SAMPLER_NICE and "nice not verified" outside src/ hits only:
- CHANGELOG.md:53 (finding 1)
- README.md:1625 (rewritten)
- tests/test_sampler.c (updated)

Nothing in tests/, scripts/ or web/ parses the old texts. run_all reads this test's output only via `sed -n 's/^TIERED_DEVIATIONS //p' | tail -1` (run_all.sh:311), which the daemon-log echo in cleanup cannot produce. No other test launches via systemd-run.

### Test script exit paths
Every path to 80 checked (VERIFIED by reading tests/test_cross_validate_tiered.sh:223-268, 415-613):
- postmaster group unknown (:435)
- v1 CPU mount missing (:449)
- mkdir failure (:451)
- load scope not started (:471)
- invalid layout (:509 `|| exit 80`): this exits before any comparator runs, so it can never reach PASS or 79
- WORKLOAD_INVALID (unchanged)

The EXIT trap can only turn 80/0/79 into 1 when cleanup fails, which is still a failure.

Inputs that come out INVALID:
- unknown nice ("unknown: …" != -10)
- empty BACKEND_PID or MainPID 0 (the readback helper exits 1 and prints "unknown")
- missing systemd-run, or not root: the scope never writes pgbench.pid, so 80

All four transient-unit paths run cleanup_rate: success, `continue`, early `exit`, and a trapped signal. On v1, the private cgroup is removed after `wait` on the load launcher. The `${!PG@}` env forwarding covers every getenv() in src/ (all PGWT_*) plus PGPORT. Bash ≥4.4 is required for the empty-array expansions under `set -u`; EL8 ships 4.4. cpu_group_readback is built by `make build-tests` (tests/Makefile `all` includes it), and it is gitignored (tests/*).

### Brief item 5 controls: which ones were actually shown
VERIFIED against the raw logs.

Ubuntu, all three isolated:
- 5a: control-nice.log shows daemon=backend=v2 /system.slice, pgbench in a user.slice scope, nice=0; exit 80.
- 5b: control-cpu.log shows the daemon in its own service leaf vs backend /system.slice/system-postgresql.slice, nice -10, and the WARNING present; exit 80.
- 5c: control-pgbench.log shows pgbench, daemon and backend all at /system.slice, nice -10; exit 80.

EL8:
- 5b is shown (control-cpu-guard-fixed.log: daemon leaf vs postgresql-18.service leaf, nice -10, warning).
- 5a is confounded: nice=0 together with daemon /system.slice ≠ backend /.
- 5c is NOT shown: pgbench=/ equals the backend's group, not the daemon's, and the run failed on daemon≠backend.

### Ubuntu tables
VERIFIED against the raw qual logs for login-2, service-1 and service-5. The deviation, PGBENCH_TPS and four readbacks per rate match REPORT.txt exactly (4.13/3.31, 1.88/2.98, 5.44/2.24; TPS 1774.38/1755.73, 1805.18/1738.47, 1744.91/1800.13). Every run exited 0 and every layout was daemon=backend=/system.slice, pgbench in user.slice, nice -10.

Ubuntu run_all: RUN_ALL_SUMMARY status PASS, executed 238, failed 0, xpass 1 (#959). EL8 run_all: PASS, failed 0, refused 5, xpass 1 (#675 test_cross_validate_tiered 10=3.24 50=2.76 100=2.82 200=2.34, with both daemon and backend at "/").

### EL8 "host history" explanation
VERIFIED as the best-supported reading. The PIDs order the runs on that boot:

| Runs | Tracer cpu group | Postmaster/backend cpu group |
|---|---|---|
| demo-plain | / | / |
| demo-cpu, CPUShares (PG pulled into its own leaf) | own leaf | /system.slice/postgresql-18.service |
| control-nice, signal, all 10 qual (826555 → 828432) | /system.slice | / |
| run_all (875081 → 875571) | / | / |
| control-cpu guard-fixed, CPU property again (885610) | own leaf | own leaf |
| control-pgbench, signal-before-readback, all 20 starts (885743 → 888616) | /system.slice | / |

The probes in el8-lazy-realization.txt and el8-scope-order.txt (CPU-property and CPUAccounting units) also moved the postmaster earlier.

So the apart state followed a CPU-property unit both times, and both of those units were the campaign's own item-2 demo and item-5b control. The test's own v1 pgbench placement and its unit start order are NOT equally consistent as sole causes: run_all used the identical script, placement and start order and got both at "/".

What reset the state between the qualification and run_all is unknown (INFERRED: systemd's lazy trimming of the system.slice cpu cgroup; not shown). The ten EL8 qualification draws therefore describe "EL8 shortly after a CPU-property unit", not a clean v1 host. A clean v1 host has n=1 (run_all, valid, passed).

## What I did not check
- I did not build or run test_sampler.c (it needs linux/capability.h) or any BPF or live test. I ran no `make check` and no box-check (per my brief).
- I did not reproduce any remote result. All Linux claims above come from reading the implementer's raw logs.
- systemd 239 vs 255 unit-GC timing: could `systemctl list-units` right after `systemctl stop` still list a `--collect` unit, giving a false "cleanup left unit loaded" exit 1? Not observed in about 60 cleanups in the logs, and not tested.
- Behaviour under the real GitHub actions.runner service unit, as opposed to the implementer's systemd-run wrapper.
- cgroup namespaces and containers, beyond the fixture showing that ".." paths give UNKNOWN.
