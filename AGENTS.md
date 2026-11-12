# Codex implementation agreement

Read the supplied brief first, then `docs/REWORK_PLAN_2026-11-11.md` method rules 1–11
and `CLAUDE.md` for the shared owner agreement. The rework is the current work.
Start only the authorised task; respect Excluded, Done, Budget and Report by.
Stop at the rounds or wall-clock limit and report partial results and remaining work.
The dispatch wrapper requires `scripts/lead/check-brief.sh` to pass for the exact brief.
Every referenced issue needs pasted live comments and a quoted CONTROLLING SCOPE DECISION.

`contracts/` is read-only for implementation: never change contracts, independent
expected answers or tolerances to fit code. CI enforcement lands on another branch.
Use only `Refs #N` in commit messages and PR text; never use Fixes/Closes/Resolves,
even negated. Write the subject, blank line and body to `.codex-commit-msg`;
the wrapper commits the working tree. Do not run git add or git commit in its sandbox.

Keep scratch work and notes outside the repository; remove scratch files before exit.
Write nothing under `tests/results/`. Never commit locally generated PNGs to
`tests/web_snapshots/`; baselines come only from CI. Run nothing eBPF-related on macOS.
Run the brief's checks, preserve existing guards, and paste commands/results as evidence.
Do not bypass push guards or the machine-wide check lock. Do not open a PR unless authorised.
Stop and report when done or when a budget, exclusion or failed check prevents completion.
