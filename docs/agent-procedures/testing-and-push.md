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

  Temporary entry `test_init_hint_recovery|959|ubuntu@6.8|exit=93` accepts only
  the diagnosed K1/K2/K3 classes by exact signature, in either recovery arm and
  including postmaster children the test did not create, within a shared cap of
  `KNOWN_CLASS_CAP = 2` PIDs per run, after the remaining checks and the
  skipped-store control pass. Every other failure stays red. Remove the entry
  and exception in the PR that fixes #959 (owner, #933 comment 6984201938).
- Never commit locally generated PNGs to `tests/web_snapshots/`; baselines are
  regenerated only by CI's `snapshots` job.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#snapshot-provenance`)
- Add C unit tests to `tests/unit_tests.list` (single source of truth); put UI
  logic in pure builders with a `tests/web_unit/*.test.mjs` case.
- Use `[skip ci]` only for docs-only commits.
- Before every push, run `make check-fast`; its stamp satisfies the pre-push hook.
  It runs the cheap guards and the slow suites whose declared globs match paths
  changed since the merge base with `origin/master`: branch commits, staged and
  unstaged edits, and untracked files that Git does not ignore. A skipped suite
  still runs in CI's `full-check`, which runs everything and must pass to merge.
  Missing/unusable merge-base history (including a shallow clone) or failure to
  enumerate paths runs every slow suite and prints the reason. Set
  `PGWT_CHECK_FAST_ALL=1 make check-fast` to force every slow suite locally;
  `make check` always runs the full local tier.
  Never bypass the guard (`PGWT_SKIP_PUSH_GUARD` is for humans). `make check`
  itself queues for a machine-wide lock through `scripts/check-lock.sh`; never
  bypass it (`PGWT_SKIP_CHECK_LOCK=1` is for humans).
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#check-serialization`)
