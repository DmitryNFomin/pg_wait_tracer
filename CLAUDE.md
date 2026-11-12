# pg_wait_tracer — working agreement for agents

Read [the accepted rework plan](docs/REWORK_PLAN_2026-11-11.md), especially method rules 1–11, first.
The rework is the current work; start only the owner-authorised slice or named task.

## RULE 0 — no status report reaches the owner unaudited

Before any owner status/progress report, run `scripts/lead/audit-report.sh <draft-file>`; fix BLOCK and paste SEND.
Paste command/output for every count, absence, permissions and running claim; otherwise label `UNVERIFIED:`.
One denied operation proves one failed attempt. Absence claims need non-vacuous `SEARCHED:` and `NOT SEARCHED:` within 8 lines.
Use this turn's `scripts/lead/held.sh <path>...` output for HELD; FREE work not running is READY-NOT-STARTED by my choice.
Name the top ledger item not running, with held output or “my choice”.
Read [reporting procedures](docs/agent-procedures/reporting.md) before drafting reports or drawing conclusions from probes.

## RULE 1 — advisers first, the owner last (#870)

Ask both Fable (`.claude/agents/adviser.md`) and Astra in parallel for design/contract decisions, new failure modes or owner questions.
Routine readiness, ordering and test-log numbers need no adviser: contracts, differential checks and mutants decide.
One written question per fresh adviser session, one answer, then stop; never resume or advise/review your own code.
Ask the owner only for unresolved adviser disagreement, disagreement with both, or owner decisions (money, machines, release, exclusions).
Read [adviser procedures](docs/agent-procedures/advisers.md) when consulting advisers or reporting a failure.

## RULE 2 — never ship a known defect

Wrong numbers, divergent paths, mismatched percentage bases or disagreeing views must be fixed, even on green branches.
Only an explicit owner exclusion in his words on the issue permits shipping; caveats and “better than master” do not.
Two views of one quantity must agree; inability to report matching numbers is a product defect.

### Release tag criteria

`tag-blocker`: an open defect making the tag wrong: (a) an actionable number is wrong or two views disagree;
(b) a real UX path is wrong, misleading or unusable (dead control, raw token/null, unrecovered error, contradictory label);
(c) a documented guarantee is false; (d) a gate relied on for (a)–(c) cannot detect its own failure case.
Not tag-blockers: process tooling/dashboards, coverage for unshipped defects, documentation-only fixes.

## The three commands

| Command | Runs where / evidence | When |
|---|---|---|
| `make check` (`check-fast` = seconds) | Mac: builders, Go, Python, UI/chaos vs mock server | Before every push; guard enforces stamp |
| `make box-check [OS=el8\|el9\|ubuntu] [PG=13\|16\|17\|18]` | x86 Linux: build, C, synthetic, protocol, real PG + live UI | Before every PR; el8 for kernel/libbpf/layout; `EPHEMERAL=1` only by VM rules below |
| `make ui-gallery [BASE=ref]` | Mac: `tests/results/ui_gallery/index.html` + `summary.json` | Every PR touching `web/` |

## Definition of done

The brief's authorised scope, exclusions, commands/evidence, rounds, wall-clock budget and report deadline govern the unit.
For PR readiness: start box-check first, run full make check concurrently; paste run_all summary and live-UI verdict in the PR.
CI-infrastructure-only work may substitute real gate-box workflow evidence; subsequent docs/test-only rounds may reuse evidence under the procedure's exclusions.
For UI changes, paste gallery counts and justify each changed/added/removed cell. Get fresh independent reviewer approval or list owner design questions.
Run test-registration and runner-registration guards; verify box log commit and tree hash against the tested tree. Use `/pr-ready`.
Read [release procedures](docs/agent-procedures/release.md) before gates/readiness: evidence paths, exact exemptions and snapshot/VM promotion rules.

## Owner rules

The main session runs Opus, plans/spawns/judges, and writes no feature code bigger than a one-liner.
1. `codex` writes the code, invoked as `codex exec` with no `-m` flag (configured sol 6.1); fresh context per PR.
2. Claude agents review/challenge; never the code's author.
3. Astra writes designs, invoked as `codex exec -m gpt-6-astra`; Fable challenges designs.
`reviewer` = **Sonnet** (high effort), Opus for `src/` logic; `ui-reviewer` = Sonnet; Explore/triage: Sonnet or Haiku.
Reviewers: `.claude/agents/reviewer.md` and `ui-reviewer.md`; Fable advises, implements only explicit src escalation after a failed Codex round.
Apply RULE 1 to doubts/uncertainty/weird results; its narrowed scope supersedes the former blanket adviser instruction.
Start only accepted-plan or owner-named work. Agreement with a proposal is not “start now”; free capacity authorises nothing.
One unit, one review round; READY-with-nits ships with nits listed; second review only for a code blocker and recorded as a sizing miss.
Two strikes: after two failed reviews, rewrite the brief for a fresh implementer; no third correction.
Cap active implementation at three; no overlapping files/tests; one UI branch at a time (Go-only exempt).
Use isolated worktrees; retain until PR open. Give a brief path, not duplicated text; report by deadline even if incomplete.
Validate every brief with `scripts/lead/check-brief.sh`; paste live issue comments and a quoted CONTROLLING SCOPE DECISION.
`codex-task.sh` requires a recent exact-content pass; on wall-clock expiry it stops Codex, commits/fetches existing work and exits 27 BUDGET_EXCEEDED.
One background wait for box runs; check once after report deadline, never poll agents/reviewers; agent reports ~15 lines, owner status five unless a decision/plan change.
On READY arm auto-merge that turn; do not push after READY, watch CI or merge manually. Reuse fresh evidence; rerun only missing/stale/contradictory evidence.
Read [dispatch/review procedures](docs/agent-procedures/dispatch-review.md) before splitting, spawning, escalation or review; the plan and RULE 1 govern superseded wording.

Before creating a VM, check both `pgwt-gate` and `pgwt-gate-2`; use a free box. Ephemeral only if both busy; report why.
Reuse one throwaway VM per agent across rounds (`KEEP=1`); delete by id with `tests/hetzner-vm.sh delete <id>` and paste deletion output.
Sweep is only for stale VMs (>6h default): never persistent boxes, very young or unknown-age VMs; cutoffs <1h require human override.
Never use sweep cutoffs to remove your VM. Never commit local PNGs to `tests/web_snapshots/`; only CI regenerates baselines.
Never mask runner noise with retries/tolerances/confirmation loops; real filed product bugs alone qualify for KNOWN_FAILING; review every xpass.
Add C tests to `tests/unit_tests.list`, pure UI builder tests to `tests/web_unit/`; `[skip ci]` is docs-only.
Push only with a passing make check stamp; never bypass push guard or machine-wide check lock (override variables are human-only).
Read [testing/push procedures](docs/agent-procedures/testing-and-push.md) for KNOWN_FAILING scopes, evidence reuse, cleanup and guard details.

## Claude hooks

Registered in `.claude/settings.json` (`*` means no matcher); the Stop sweep hook is unregistered by the plan.

| Event | Matcher | Command |
|---|---|---|
| `PreToolUse` | `Bash` | `scripts/hooks/push-guard.sh` |
| `SubagentStart` | `*` | `scripts/lead/agent-start-hook.sh` |
| `SubagentStop` | `*` | `scripts/lead/agent-stop-hook.sh` |

## Local setup (one-time)

Back up/move personal .git/hooks before `git config core.hooksPath scripts/git-hooks` (make check sets it if unset).
Run nothing eBPF-related on macOS.
```
brew install node go
python3 -m pip install --user playwright==1.60.0 websockets pillow numpy
python3 -m playwright install chromium
export PGWT_BOX=root@<gate-box-ip> # optional PGWT_BOX_EL8, PGWT_BOX_EL9
```
`.python-version` is excluded locally; worktrees use global pyenv default or explicit PYENV_VERSION.
Read [hooks/setup procedures](docs/agent-procedures/hooks-and-setup.md) when configuring hooks or interpreting hook behavior.
Read [earlier context](docs/agent-procedures/context.md) only for the old handover, operating-model history or repository layout.
