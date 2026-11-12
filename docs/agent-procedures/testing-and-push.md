# Testing, VM cleanup and push procedure

## Rules

- Branch `agent/<slug>`; one task per branch; use an isolated git worktree
  (`EnterWorktree`). Keep the implementer's worktree until its PR is open.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#worktree-retention`)
- Reviewers must reuse `make box-check`/`make ui-gallery` evidence newer than
  the last code commit. Re-run only if missing, stale or contradicting the
  implementer's claim; state which in the PR body.
- Reuse **one throwaway VM per agent** across rounds (`KEEP=1`); delete it
  when finished and paste deletion output in the report. Never create one per
  iteration or leave it for the 6h sweep. Delete by id only:
  `tests/hetzner-vm.sh delete <id>` (API-confirmed deletion); never use
  `MAX_AGE_HOURS=0 make hetzner-sweep` or any cutoff to remove a specific VM.
  `tests/hetzner-sweep.sh` refuses cutoffs below 1h (`--force-all` overrides);
  even then, the script never deletes VMs younger than a few minutes or of unknown
  age. The sweep is a janitor for stale VMs, not a tool for removing one specific machine.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#vm-cleanup`)
- Never harden tests against runner noise with retries, wider tolerances or
  confirmation loops. If a test is red only on shared runners, say so in the
  PR; it belongs on the dedicated gate box.
- Use `tests/run_all.sh`'s `KNOWN_FAILING` (test name → tracking issue number)
  only for real, filed product bugs being fixed in `src/`, never timing/runner
  noise; move or investigate noise instead. The runner executes listed tests every
  time; expected failures and unexpected passes (`UNEXPECTED PASS`) both count as
  `known-failing` without failing the gate; passes also count as `xpass`. Review
  every `xpass` against its issue, never assume staleness. Exit 126 (not executable)
  or 127 (command/file not found) fails the gate even for listed tests.
  Optional scopes (`name|issue|os@kernel-major.minor|exit=NN`, either predicate may
  be omitted) match the runner's `/etc/os-release` and `uname -r`; scoped entries
  require the expected nonzero exit code and any other failure fails the gate;
  outside that scope the test must pass normally.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#known-failing-invocations`)
- Never commit locally generated PNGs to `tests/web_snapshots/`; baselines are
  regenerated only by CI's `snapshots` job.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#snapshot-provenance`)
- Add C unit tests to `tests/unit_tests.list` (single source of truth); put UI
  logic in pure builders with a `tests/web_unit/*.test.mjs` case.
- Use `[skip ci]` only for docs-only commits.
- Push only with a passing `make check` stamp; the hook blocks pushes without one.
  Never bypass the guard (`PGWT_SKIP_PUSH_GUARD` is for humans). `make check`
  itself queues for a machine-wide lock through `scripts/check-lock.sh`; never
  bypass it (`PGWT_SKIP_CHECK_LOCK=1` is for humans).
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#check-serialization`)
