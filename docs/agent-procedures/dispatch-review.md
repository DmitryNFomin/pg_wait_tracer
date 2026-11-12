# Dispatch, models and review procedure

The rework plan method rules 1–11 and CLAUDE.md RULE 1 (#870) control.
A second review round is a recorded sizing miss; after two failed reviews,
rewrite the brief for a fresh implementer instead of a third correction.

## Who does what (main session → subagents)

The main interactive agent plans, splits, spawns and judges. It writes no feature
code bigger than a one-liner.

- **Models** are pinned in `.claude/agents/*.md`, overridden at spawn time.
  Use the cheapest model that does good work; **spend on review, economise on implementation**.
  Owner rule 2026-10-28, four operative instructions:
  1. `codex` writes the code, invoked as `codex exec` with no `-m` flag
     (`~/.codex/config.toml` already pins `model = "gpt-6.1-sol"`, so the
     default IS sol 6.1). Every PR gets a NEW `codex` agent with fresh
     context — never a resumed or reused one.
  2. Claude agents review and challenge that code. Never the agent that wrote it.
  3. Astra writes designs, invoked as `codex exec -m gpt-6-astra`. Fable
     challenges design decisions.
  4. Adviser questions follow the narrowed RULE 1 (#870): design/contract
     decisions, new failure modes and owner questions. Routine readiness,
     ordering and test-log numbers need no adviser. This replaces the former
     blanket instruction to consult both advisers on every uncertainty.
  `reviewer` = **Sonnet** (high effort), **Opus for `src/` logic** — that
  review is where silent-wrong-answer bugs get caught, so it is never
  downgraded. `ui-reviewer` = Sonnet. `Explore`/triage: Sonnet or Haiku.
  ONE review round: ship READY-with-nits with nits listed; second round only for
  a code blocker; no confirmation re-reviews. Wait for box runs with one
  `run_in_background` wait, then read results; do not loop. Reviewers stop polling agents.
  Keep agent reports at most ~15 lines.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#models-and-review`; `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#polling-cost`)
- **Fable**: use as standing adviser (design-challenge, per Models above), not
  implementer; leave implementation unused except explicit escalation for a
  `src/` problem after `codex` has already failed a round. If escalation is not worthwhile, park and report
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
- **Start only authorised work** (owner, 2026-11-11): the lead starts only work in
  the accepted plan (`docs/REWORK_PLAN_2026-11-11.md`) or work the owner named. An
  owner agreeing to a proposal is not a go for building; ask "start now?". A free
  box or a free agent slot is never a reason to start something. Check capacity
  (implementers vs cap, boxes via `uptime` plus `fuser /tmp/pgwt-box-check.lock`,
  open PRs `BEHIND`) when a unit finishes, not every turn. Determine blocking by
  shared FILES, not phase number: modules run in parallel, wiring serially.
  (supersedes the former "capacity check every turn" rule; history in
  `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#idle-capacity`)
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
- **Adviser**: Fable via `.claude/agents/adviser.md`, and
  Astra (`codex exec -m gpt-6-astra`, see Models above), consulted as RULE 1 says:
  design or contract decisions, failure modes not seen before, and anything that
  would become an owner question. One written question per adviser, a fresh
  session each time, one answer; never resume an adviser conversation (owner,
  2026-11-11: long-lived adviser sessions were the largest single cost).
  Dispatch both in parallel and do independent work meanwhile. Advisers review
  reasoning, not code; never edit, commit or create cloud resources. The lead
  may overrule advice but must say so in the report.
  (why: see `docs/AGENT_WORKING_AGREEMENT_RATIONALE.md#adviser-incidents`)

## Brief validation and budget

Use `docs/BRIEF_TEMPLATE.md` for every dispatch. Paste the live issue comments
for every referenced issue/PR, including all ids and bodies, and identify the
controlling decision with its comment id and a verbatim quote. Run
`scripts/lead/check-brief.sh <brief-file>` immediately before dispatch; passing
records bind exact bytes for at most 24 hours. Store briefs and lead state outside
the implementation tree; set `PGWT_LEAD_STATE_DIR` to an existing writable directory.

`scripts/lead/codex-task.sh` refuses unchecked/changed/stale briefs (exit 15).
The wall-clock budget runs from Codex launch, ends the whole process group at the
deadline (TERM, then KILL with at most two seconds for cleanup), and still saves,
auto-commits and fetches existing work. A verified handback exits 27 BUDGET_EXCEEDED;
salvage, commit or fetch failures retain their more specific failure codes.
The round limit is the implementer's stop condition; the wrapper launches one round
and does not retry. Hand back what exists; additional scope/rounds need a new brief.
The unregistered Stop hook is not a brief gate and records no new spawn diagnostics.
