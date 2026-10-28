# pg_wait_tracer — working agreement for agents

## RULE 0 — no status report reaches the owner unaudited. No exceptions.

Verify the lead's claims before the owner sees them. Before **any** status report,
progress summary or answer about why work is or is not happening, run:

```
scripts/lead/audit-report.sh <draft-file>     # codex re-derives every claim; returns SEND or BLOCK
```

Fix **BLOCK** drafts before sending. Paste the verdict line with the report.
For held claims, run `scripts/lead/held.sh <path>...` (prints `HELD <path> by #N`
or `FREE <path>`); use only:

- `RUNNING <item> <PR/agent> since <t>`
- `HELD <item> by #N on <files>` — paste **this turn's** `held.sh` output
- `READY-NOT-STARTED <item>: my choice — I ran <X> instead`

If `held.sh` says FREE and the item is not running, report **READY-NOT-STARTED
by my choice**, never "blocked". Every report must name the **top ledger item
not running**, with `held.sh` output or "my choice".
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#held-claims`)

Paste a command and its output for every **count**, **absence** ("exists nowhere",
"not merged", "unreachable"), **permissions** claim ("I can't"), and **running** claim.
Treat one denied operation as one failed attempt only. Without evidence, write
`UNVERIFIED:`, never a cause.

## RULE 1 — advisers first, the owner last. Never ask the owner what an adviser can answer.

Send design, scope, sequencing, approach and option questions to **both advisers first**:
`codex exec` and `.claude/agents/adviser.md`, in parallel. Ask the owner only if
the advisers disagree and the lead cannot adjudicate on evidence; the lead disagrees with both;
or the decision is the owner's (money, machines, release timing, RULE 2 exclusions).
Tell the owner what both advisers said and where the lead disagrees.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#advisers-before-owner`)

Try first; on failure, tell both advisers what was attempted and what happened.
Report failures to the owner only afterwards, with attempts, outcomes, options and
a recommendation; never a bare "this does not work, FYI". Solve it first otherwise.
If both advisers find no way, report what X, Y and Z they considered and each cost.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#failure-reports`)

## RULE 2 — never ship a known defect. "Good enough" is not a state.

Do not ship known wrong numbers, divergent paths, mismatched percentage bases or
disagreeing views, even on a green branch. Fix correctly; disclosure or caveats do
not substitute. Only an explicit owner exclusion, in his words on the issue,
permits shipping; the lead cannot grant or infer it, including from "better than master".
Treat inability to report matching numbers as a product defect. Two views of one
quantity must agree; fix the disagreement instead of asking which wrong number to display.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#matching-numbers`)

PostgreSQL eBPF tracer: C/BPF daemon, `pgwt-server`, Go WebSocket bridge,
ECharts/uPlot UI. Plan/status: `docs/ROADMAP_AND_STATUS.md`. Run nothing eBPF-related on macOS.

## The three commands

| Command | Runs where / evidence | When |
|---|---|---|
| `make check` (`check-fast` = seconds) | Mac: Node builders, Go bridge, py_compile, Playwright UI/chaos vs `tests/mock_server.py` | Before every push; push guard enforces it |
| `make box-check [OS=el8\|el9\|ubuntu] [PG=13\|16\|17\|18]` | x86 Linux over ssh (`$PGWT_BOX`, `$PGWT_BOX_EL8`…): build, C units, synthetic data, protocol drift, real-PG capture (`tests/run_all.sh --require-live`, `tests/ui_live_smoke.sh`, `tests/results/ui_live/summary.json`) | Before every PR; use `OS=el8` with `$PGWT_BOX_EL8` for kernel/libbpf/layout changes |
| `make box-check EPHEMERAL=1 [PG=…] [KEEP=1]` | Private Hetzner VM from `pgwt=gate-snapshot`, same live tier; automatic deletion unless `KEEP=1` prints the delete command | Every agent Ubuntu run, including final pre-PR; reserve persistent boxes for CI self-hosted jobs; `EPHEMERAL=1` supports `OS=ubuntu` only (`scripts/box-check.sh` refuses others) |
| `make ui-gallery [BASE=ref]` | Mac: all UI snapshot cells → `tests/results/ui_gallery/index.html` + `summary.json` | Before every PR touching `web/` |

(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#ubuntu-vm-evidence`)

**Persistent boxes FIRST; throwaway VMs only as exceptions.** Before creating any
VM, check both `pgwt-gate` and `pgwt-gate-2`; use a free box. Use `EPHEMERAL=1`
only when both are busy with CI or another agent holding the flock; report which
box was busy and why.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#persistent-boxes-first`)

Logs: `tests/results/box-check-*.log` (`box-check-ephemeral-*.log` for `EPHEMERAL=1`),
`tests/results/ui_gallery/`, `tests/results/ui_live/summary.json`.

`make hetzner-sweep` deletes `pgwt=ephemeral` VMs older than 6h (automatic at
every `box-check` start). The script never touches `pgwt-gate` or `pgwt-gate-2`
by name, regardless of label. It refuses cutoffs below 1h unless `FORCE_ALL=1`;
regardless of cutoff, it never sweeps VMs younger than a few minutes or with
unknown age (missing/unparseable `created=`), which it never treats as infinitely
old (issue #162). Remove your own VM by id:
`tests/hetzner-vm.sh delete <id>`, never by sweep cutoff.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#vm-cleanup`)

## Definition of done

Declare a PR ready only with ALL evidence in its body:

0. **Start `box-check` FIRST; run `make check` while it runs** (`make check`: median ~7 min, n=3; well under the 40-min box run, so serial execution wastes the check's wall time).
   (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#parallel-checks`)
1. Full `make check` (not `--fast`) passed on the final tree.
2. `make box-check` passed; paste the run_all summary (last ~20 lines), including
   the live-UI-smoke verdict (`tests/results/ui_live/summary.json`).
   CI-infrastructure-only branches (no `src/` or test-assertion changes) may
   substitute real `gh workflow run` gate-box evidence; state this in the PR body.
   After the first passing `box-check`, docs-only review rounds or `tests/`-only
   rounds touching no live-tier test may skip re-runs; name each round and why in
   the PR body. Never exempt `src/`, `web/`, `tests/run_all.sh`,
   `tests/unit_tests.list`, or any live-tier test; if unsure, run `box-check`.
   `.github/workflows/ci.yml` narrows tiers by changed files; CI still runs in
   full on every PR, and merge gates are unchanged.
3. If `web/static/` or `tests/web_snapshots/` changed, run `make ui-gallery`;
   list `summary.json` counts and every `changed`/`added`/`removed` cell with
   a one-line justification each.
   (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#ui-branch-scope`)
4. Get fresh reviewer approval (`.claude/agents/reviewer.md`; `ui-reviewer.md`
   for UI), or list open questions under "Design questions for the owner".
   Give the owner only those questions to resolve.

Use `/pr-ready` to run this sequence.

## Who does what (main session → subagents)

The main interactive agent plans, splits, spawns and judges. It writes no feature
code bigger than a one-liner.

- **Models** are pinned in `.claude/agents/*.md`, overridden at spawn time.
  Use the cheapest model that does good work; **spend on review, economise on implementation**.
  `implementer` = **Sonnet** for UI, tests, tooling and docs; **Opus** for anything
  under `src/` (BPF, capture, discovery, backend layout, accounting) and for
  design-heavy UI after a Sonnet attempt failed.
  `reviewer` = **Sonnet** (high effort), **Opus for `src/` logic** — that
  review is where silent-wrong-answer bugs get caught, so it is never
  downgraded. `ui-reviewer` = Sonnet. `Explore`/triage: Sonnet or Haiku.
  ONE review round: ship READY-with-nits with nits listed; second round only for
  a code blocker; no confirmation re-reviews. Wait for box runs with one
  `run_in_background` wait, then read results; do not loop. Reviewers stop polling agents.
  Keep agent reports at most ~15 lines.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#models-and-review`; `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#polling-cost`)
- **Fable**: use as standing adviser, not implementer; leave implementation
  unused except explicit escalation for a `src/` problem after an Opus
  implementer has already failed a round. If escalation is not worthwhile, park and report
  the task to the owner; never silently retry or downgrade. Record it in memory
  `fable-parked-tasks`. Check the budget before repeating "out of Fable tokens".
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#fable-history`)
- **Main session model**: run Opus (`/model`), never Fable. Read 15-line reports,
  decide ready/back, arm auto-merge and spawn. Minimise turns: act on agent reports;
  check once after a report-by deadline; never on "still running" notifications
  or mid-flight status checks. Do not read a diff when the reviewer's report
  answers the question. Return findings touching signal handling, loops,
  accounting or the fail-safe rule to the implementer regardless of reviewer label.
  Delegate web searches, `gh api` reads, price lookups and code archaeology to
  Haiku/Sonnet; get one short report. The orchestrator does not research.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#orchestrator-turns`)
- **Capacity check EVERY turn, before replying to an agent report**: check
  active implementers vs cap; codex (free immediately on return); both boxes
  (`uptime` plus `fuser /tmp/pgwt-box-check.lock`); advisers; open PRs needing
  `gh pr update-branch` for `BEHIND`. Answer AND fill idle capacity in the SAME
  turn. Do not count blocked work as idle. Determine blocking by shared FILES,
  not phase number: modules run in
  parallel, wiring serially. A box excluded from general gating remains available
  to reproduce the very issue that excludes it.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#idle-capacity`)
- **Reports to the owner: FIVE LINES at most** — merged / in flight / blocked /
  needs you. Use long form only for owner decisions or plan-changing findings.
  Put bug narratives in the issue tracker; link them.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#owner-report-limit`)
- **Splitting**: one roadmap item = one issue = one branch = one implementer.
  Split only disjoint files AND disjoint tests; never split a shared file.
  Cap actively implementing tasks at **three** (writing/running code); exclude
  agents idling during CI, `box-check` or review. `scripts/check-lock.sh` (`check:`
  target) serializes Mac `make check` machine-wide. Apply no Mac-derived gate-phase
  cap (waiting on/running `make check`); do not invent a "two in gate phase" limit.
  Before spawning, list touched files, compare every in-flight branch and refuse
  overlap up front.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#splitting-and-gate-capacity`; `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#shared-file-collision`)
  Allow at most **ONE** branch touching `web/static/` or `tests/web_snapshots/`
  at a time; Go-only `web/` changes are exempt. Send work over ~a day to a `Plan`
  agent first; run steps sequentially unless the plan establishes independence.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#ui-branch-scope`)
- **Spawning**: always `isolation: "worktree"`. Include issue text, acceptance
  criteria, required `make` targets, "stop and report, do not open the PR", and
  "report by T+N minutes even if incomplete" in the contract. Treat absent
  notifications as the hang signal; check once after the deadline instead of
  watching the stream. For a contract written to a file, pass only its path;
  do not paste its text too.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#spawn-contracts`)
- **Review chain**: (1) `make check`/`box-check`/`ui-gallery` produce binding evidence files;
  (2) a fresh `reviewer` (+ `ui-reviewer` for `web/`) reads code/evidence and writes
  the PR body; return blockers via SendMessage; (3) the main agent reads the
  report, not the diff, and decides ready/back/ask owner. Send only "Design
  questions for the owner" to the human. On READY, arm auto-merge in the same
  turn; do not watch CI or merge manually. Do not push after READY.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#auto-merge-and-ready`)
- **Three evidence guards a reviewer runs before declaring READY**:
  - Run `scripts/check-test-registration.sh`: it fails and names any merge-base
    entry missing from `tests/unit_tests.list` or `tests/Makefile`'s `TESTS` list
    in the current tree (append-only invariant).
    Mac-side, seconds, no box. Self-tests: `tests/test_check_test_registration.sh`.
  - Match `scripts/box-check.sh` log-header `commit=<git rev-parse HEAD>` and
    `tree-hash=<scripts/tree-hash.sh>` against the PR head before trusting the log;
    these identify the exact tree synced by the immediately following rsync.
  - Run `scripts/check-runner-registration.sh` (issue #433) against THIS repo
    manually, like `check-test-registration.sh`; `make check` runs only its
    synthetic self-test (`tests/test_check_runner_registration.sh`). Mac-side,
    seconds, no box. The script fails if a test actually invoked by
    `tests/ci_smoke.sh` is not also invoked by `tests/run_all.sh`. Its orphan scan
    fails if an on-disk `tests/test_*.py`/`tests/test_*.sh` is invoked by none of
    `tests/run_all.sh` (directly, `LIVE_TESTS`, or line-by-line
    `tests/unit_tests.list`), `tests/ci_smoke.sh`, `scripts/check.sh`, or root
    `Makefile`, unless already named with a reason in
    `tests/runner_registration_exemptions.list`.
    Both checks count actual invocations, not comments/mentions; runner globs such as
    `tests/web_unit/*.test.mjs` register every matching file. Leave live guard
    execution as a manual reviewer step; let a human judge deliberate manual-only tools.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#evidence-guard-incidents`; `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#runner-registration-gaps`)
- **Adviser**: consult BOTH by default — primary codex via `codex exec`, standing
  second Fable via `.claude/agents/adviser.md`. Keep one Fable conversation per
  session; resume with SendMessage, never respawn. Consult on every found issue,
  plan and numeric claim BEFORE acting and BEFORE reporting. Consult both before
  accepting/rejecting review verdicts or returning branches; owner-facing numbers,
  measurements or facts; plans, ordering or scheduling; infrastructure/cost
  recommendations; retractions/corrections. Do not skip for cost: dispatch in
  parallel, do independent work meanwhile, block only dependent work. Advisers
  review reasoning, not code; never edit, commit or create cloud resources.
  The lead may overrule advice but must say so in the report.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#adviser-incidents`)

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

## Local setup (one-time)

Activate the pre-push stamp guard per clone (covers linked worktrees;
`make check` activates it if unset). Before setting `core.hooksPath`, back up
or move personal `.git/hooks/` hooks to `scripts/git-hooks/`.
(why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#hook-setup`)

```
git config core.hooksPath scripts/git-hooks
```

```
brew install node go
python3 -m pip install --user playwright==1.60.0 websockets pillow numpy
python3 -m playwright install chromium
export PGWT_BOX=root@<gate-box-ip>        # optional: PGWT_BOX_EL8, PGWT_BOX_EL9
```

`.python-version` is untracked via `.git/info/exclude`; new worktrees get the
pyenv pin only from the global default or explicit `PYENV_VERSION`, never checkout.

## Layout

- `src/` daemon + BPF (`src/bpf/`), `src/server.c` = `pgwt-server`
- `web/` Go bridge; `web/static/` UI — `views/` per-tab, `dev/gallery.html` fixture gallery
- `tests/` everything: `run_all.sh` (Linux master runner), `ci_smoke.sh` (real-PG
  capture), `test_web_ui*.py` (Playwright), `web_unit/` (Node), `web_snapshots/` (baselines)
- `docs/VISUAL_CHECKLIST.md` — the seven words a UI reviewer grades against
- `.github/workflows/ci.yml` gating; `nightly.yml` OS matrix
