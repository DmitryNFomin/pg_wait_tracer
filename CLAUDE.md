# pg_wait_tracer — working agreement for agents

eBPF wait-event tracer for PostgreSQL (C + BPF daemon, `pgwt-server` analysis
backend, Go WebSocket bridge, ECharts/uPlot web UI). Plan and status:
`docs/ROADMAP_AND_STATUS.md`. Nothing eBPF-related runs on macOS.

## The three commands

| Command | Runs where | What | When |
|---|---|---|---|
| `make check` (`check-fast` = seconds) | this Mac | Node builder tests, Go bridge, py_compile, Playwright UI + chaos suites vs `tests/mock_server.py` | before every push — the push guard enforces it |
| `make box-check [OS=el8\|el9\|ubuntu] [PG=13\|16\|17\|18]` | x86 Linux box over ssh (`$PGWT_BOX`, `$PGWT_BOX_EL8`…) | Linux build, C units, synthetic-data tests, protocol drift, real-PG capture (`tests/run_all.sh --require-live`, incl. the live-UI-smoke walk — `tests/ui_live_smoke.sh` / `tests/results/ui_live/summary.json`) | before every PR; `OS=el8` when touching kernel/libbpf/layout code |
| `make box-check EPHEMERAL=1 [PG=…] [KEEP=1]` | throwaway Hetzner VM created from the `pgwt=gate-snapshot` image, always deleted at the end (`KEEP=1` leaves it up and prints the delete command) | the same live tier as above, on a private one-run VM — no `$PGWT_BOX`, no flock contention with other agents | **every agent run on Ubuntu, including the final pre-PR one** — the throwaway VM is the same Hetzner machine class as the gate box (103b1a0 matched cpx42→cx33), so for the Ubuntu tier it is the same evidence. The two persistent boxes are reserved for CI's self-hosted jobs: an agent running there queues behind CI on the same `flock`, which is most of the 29–117 min CI spread (n=9 runs). **`EPHEMERAL=1` supports `OS=ubuntu` only** — `scripts/box-check.sh` refuses anything else, because no `pgwt=gate-snapshot` image exists for el8/el9. So kernel/libbpf/layout work still runs `OS=el8` against `$PGWT_BOX_EL8`: 103b1a0 matched the machine class, never the kernel, and el8 is exactly the tier where that difference is the point |
| `make ui-gallery [BASE=ref]` | this Mac | before/after screenshots of every UI snapshot cell → `tests/results/ui_gallery/index.html` + `summary.json` | before every PR that touches `web/` |

**Persistent boxes FIRST; a throwaway VM is the exception** (owner rule
2026-09-28: *"use second gate box for next tests as soon as it's idle, do not
create new machine, create a new ONLY if it's needed and gate box busy with CI
or other tests"*). Both `pgwt-gate` and `pgwt-gate-2` are paid for whether or
not they run anything — a stopped Hetzner server still bills, so an idle box is
pure waste. Before creating any VM, check both boxes; use one if it is free.
`EPHEMERAL=1` is legitimate only when **both** are genuinely busy (CI job or
another agent's run holding the flock), and the report must say which box was
busy and why. On 2026-09-28 thirteen throwaway VMs were created in a day, each
re-provisioning PostgreSQL and pgbench from scratch, while both gate boxes sat
idle for nine to thirteen hours — and the multi-minute provisioning window was
itself the cause of three failed Mac-side attempts, because the display slept
and the session locked while it ran. Using a ready box removes the cost and the
failure mode together.

Logs: `tests/results/box-check-*.log` (`box-check-ephemeral-*.log` for `EPHEMERAL=1`), `tests/results/ui_gallery/`, `tests/results/ui_live/summary.json`.

`make hetzner-sweep` deletes any `pgwt=ephemeral`-labelled Hetzner VM older than
6h (also runs automatically at the start of every `box-check`); the
persistent gate boxes (`pgwt-gate`, `pgwt-gate-2`) are never touched by it,
by name, regardless of label. A cutoff below 1h is
refused (`FORCE_ALL=1` to override) and a VM younger than a few minutes, or
whose age can't be determined at all (missing/unparseable `created=` label —
unknown age is never treated as "infinitely old"), is never swept regardless
of cutoff (issue #162) — to remove your own VM, delete it by id
(`tests/hetzner-vm.sh delete <id>`), never via the sweep's cutoff.

## Definition of done

A PR is ready when ALL of these are true and the evidence is in the PR body:
0. **Start `box-check` FIRST and run `make check` while it is running.** They
   use different machines and are independent (the check's flock is local to
   this Mac, the box run holds its own remote flock, and `box-check.sh`
   excludes `.pgwt-check.stamp` from its rsync), so running them in series
   adds the check's whole wall time to every PR for nothing. Measured
   2026-09-27 on this Mac under concurrent agent load: 6, 9 and 7 minutes
   (n=3, median 7) — `scripts/check.sh`'s "~4 min" header describes an
   unloaded run.
1. `make check` passed (full, not `--fast`) on the final tree.
2. `make box-check` passed — paste the run_all summary (last ~20 lines), which
   includes the live-UI-smoke one-line verdict (`tests/results/ui_live/summary.json`).
   Carve-out: a branch touching only CI infrastructure (no `src/` change, no
   test-assertion change) may substitute real `gh workflow run` evidence on
   the gate box for `make box-check` — state that substitution explicitly in
   the PR body. After a branch's first passing `box-check`, review rounds for
   docs-only changes or `tests/`-only changes touching no test the live tier
   executes may skip re-runs; name each exempted round and why in the PR body.
   Never exempt changes to `src/`, `web/`, `tests/run_all.sh`,
   `tests/unit_tests.list`, or any live-tier test; if unsure, run `box-check`.
   `.github/workflows/ci.yml` already narrows its tier by changed files; CI
   still runs in full on every PR, and merge gates are unchanged.
3. If `web/` changed: `make ui-gallery` ran; the `summary.json` counts and every
   `changed`/`added`/`removed` cell are listed with a one-line justification each.
4. A fresh **reviewer** agent (`.claude/agents/reviewer.md`; `ui-reviewer.md`
   for UI) approved, or its open questions are listed under "Design questions
   for the owner". Those questions are the ONLY thing the owner should have to
   think about.
Use `/pr-ready` to run this sequence.

## Who does what (main session → subagents)

The interactive session is the **main agent**: it plans, splits, spawns, and
judges. It writes no feature code itself for anything bigger than a one-liner.

- **Models** are pinned in `.claude/agents/*.md`, overridden at spawn time
  (owner rules 2026-09-20 and 2026-09-25: cheapest model that does good work,
  and **spend on review, economise on implementation** — a blocker caught in
  review costs one message, one that reaches CI costs an hour of serialised
  gate-box time):
  `implementer` = **Sonnet** for UI, tests, tooling and docs; **Opus** for
  anything under `src/` (BPF, capture, discovery, backend layout, accounting)
  and for design-heavy UI after a Sonnet attempt failed.
  `reviewer` = **Sonnet** (high effort), **Opus for `src/` logic** — that
  review is where silent-wrong-answer bugs get caught, so it is never
  downgraded. `ui-reviewer` = Sonnet. `Explore`/triage: Sonnet or Haiku.
  ONE review round: READY-with-nits ships with the nits listed; a second round
  only for a code blocker; no confirmation re-reviews. Agents wait for box runs
  with one `run_in_background` wait, then read the result. Do not loop: this
  harness blocks long standalone `sleep` commands, and each short backgrounded
  wait emits a `stopped with background work` notification that makes the
  orchestrator re-read its full context. On 2026-09-29 one polling loop sent
  15 such notifications in 2h10m, costing ~9M cache-read tokens (~5% of the
  day's spend). Reviewers stop agents that poll.
  Reports are at most ~15 lines.
- **Fable**: not an implementer, but it **is the standing adviser** (see the
  Adviser bullet below), and that is its normal, expected use. As an
  *implementer* it stays unused (owner rule 2026-09-25): on #128 it needed two
  blocker rounds, still failed CI, cost more than every Sonnet agent of that
  night combined, and died mid-task on its own usage limit; the main session
  running on Fable cost more than all subagents together. It remains available
  as an explicit escalation for a `src/` problem an Opus implementer has
  already failed a round on. When such a task is judged not worth the
  escalation it is **parked and reported to the owner**, never silently
  retried or quietly downgraded — parked list: memory `fable-parked-tasks`.
  Fable's budget was exhausted on 2026-09-25 and had **reset by 2026-09-27**,
  since when it has been in continuous use as the adviser; check before
  repeating "out of Fable tokens".
- **Main session model**: the orchestrator runs on **Opus** (`/model`), never
  Fable — it reads 15-line reports, decides ready/back, arms auto-merge and
  spawns, and every one of its turns re-reads the whole context, so its model
  is the largest single token cost. It minimises its own turns: it acts on
  agent reports and checks once when a report-by deadline passes, never on
  "still running" notifications or mid-flight status checks, and it never
  reads a diff itself when a reviewer's
  report answers the question. Counterweight so quality does not slip: a
  finding that touches signal handling, loops, accounting or the fail-safe rule
  goes back to the implementer whatever label the reviewer gave it.
  The orchestrator does not research: web searches, `gh api` reads, price
  lookups and code archaeology go to a Haiku or Sonnet agent and return as one
  short report. Each orchestrator tool call costs a full context re-read of a
  conversation that only grows; on 2026-09-29, 57% of its turns followed its
  own tool calls.
- **Capacity check EVERY turn, before replying to an agent report** (owner rule
  2026-10-09: *"why I asking you every time find that there are idle resources and
  waiting for it work? Any good reason?"* — there was none). The orchestrator is
  event-driven by default: each report triggers a response about that report, and
  nothing asks "what else could be running?", so idle capacity persists until the
  owner notices. It happened three times on 2026-10-09 — one implementer against a
  cap of three while two independent modules waited; codex idle twice with unblocked
  work available; gate-2 at load 0.02 while gate-1 sat at 7.30 with its lock held.
  Before answering any report, check: implementers running vs the cap; codex (free
  the moment it returns); both boxes (`uptime` plus `fuser /tmp/pgwt-box-check.lock`);
  advisers; and open PRs for a `BEHIND` needing `gh pr update-branch`. Then answer
  the report AND fill what is idle in the SAME turn. **Blocked is not idle — the
  test is whether the work shares FILES, not whether it shares a phase number:
  modules are parallel, wiring is serial.** And a box excluded from gating is not
  useless: gate-2 is the right place to reproduce the very issue that excludes it.
- **Reports to the owner are FIVE LINES at most** (owner rule 2026-09-25):
  merged / in flight / blocked / needs you. Long form only when the owner has
  to decide something, or when a finding changes the plan. No bug narratives —
  the issue tracker holds the detail, and a link is enough.
- **Splitting**: one roadmap item = one issue = one branch = one implementer.
  Split only along an independent seam (disjoint files AND disjoint tests);
  never split a shared file; at most **three tasks actively implementing**
  (an agent idling while CI, `box-check`, or a review round runs does not
  count against the three — only agents actually writing/running code do).
  This is safe because `make check` on this Mac is now serialized machine-wide
  by `scripts/check-lock.sh` (the `check:` Makefile target): a third agent
  queues for the lock instead of piling a third concurrent Playwright/chaos
  run onto the same laptop. But the lock is a CPU cap, not a queue that costs
  nothing. Corrected 2026-10-08: the figure here used to read "3 x 22 min",
  which contradicted the measured median at the top of this file. Three agents
  in GATE phase are 3 x ~7 min (the n=3 median above), so **~21 min, under the
  40-min box run — the BOXES are the critical path, not this Mac**, at every
  agent count we actually run. So there is no Mac-derived cap on agents in gate
  phase (gate phase = waiting on or running `make check`); the lock makes them
  queue, and the queue drains faster than a box run. The cap that binds is the
  three-actively-implementing one above, plus the one-`web/`-branch rule. Do
  not re-derive a "two in gate phase" limit from this paragraph: it came from
  the wrong number. Before spawning, list the files the task will
  touch and compare them against every in-flight branch — an overlap is
  refused up front, not discovered at merge (2026-09-25: two branches both
  edited `tests/test_durability.c`, git merged them silently and the build
  broke). **At most ONE branch touching `web/` at a time**: UI branches
  collide on snapshot baselines and `tests/web_snapshots/VERSION` by
  construction, and each collision costs a manual merge plus a regeneration. Anything over ~a
  day of work goes to a `Plan` agent first; its steps run sequentially unless
  the plan shows them independent.
- **Spawning**: always `isolation: "worktree"`. The prompt is a contract:
  issue text, acceptance criteria, required `make` targets, "stop and report,
  do not open the PR", and "report by T+N minutes even if incomplete". On
  2026-09-29 every agent notification was `completed`, and 26 of 40 were
  `stopped with background work` — a live-and-polling signal. A hung agent
  emits nothing; absence is the only hang signal. Check once when the deadline
  passes instead of watching the stream. When a contract is long enough to
  write to a file, pass its path only; do not also paste its text into the
  message. The duplicate brief stays in the orchestrator's context and is
  re-read for the rest of the session.
- **Review chain**: (1) scripts — `make check`/`box-check`/`ui-gallery`
  produce files the implementer cannot argue with; (2) a fresh `reviewer`
  (+ `ui-reviewer` when `web/` changed) reads code + evidence and writes the
  PR body; blockers go back to the implementer via SendMessage; (3) the main
  agent reads the reviewer's report, not the diff, and decides ready / back /
  ask the owner. Only "Design questions for the owner" reach the human. On READY,
  arm auto-merge in that same turn; do not watch CI or merge by hand. Repository
  "Allow auto-merge" and branch protection "Require branches to be up to date
  before merging" (`strict`) were enabled 2026-09-29. Strict matters: on
  2026-09-28 #228, #229 and #233 hand-merged within 13 seconds, each having
  passed CI against base `5c5e3c66`, none tested against the others. Strict
  forces the second and third to update and re-run CI against the combined
  tree, where a shared-file break goes red. Required approving reviews is 0:
  a push after READY would merge unreviewed. The contract's no-push-after-READY
  rule is now load-bearing.
- **Adviser** (owner rules 2026-09-27 and 2026-09-29): consult BOTH by default:
  codex via `codex exec` is primary; Fable via `.claude/agents/adviser.md`
  is the standing second.
  Keep one standing Fable conversation for the whole session (resume it with
  SendMessage, never respawn). The retro found the lead's own output is the
  only artifact with no fresh reviewer — every wrong statement that reached
  the owner started there. So: the main agent consults the adviser on every
  found issue, plan and numeric claim BEFORE acting on it and BEFORE
  reporting it to the owner. Consult both BEFORE accepting or rejecting a
  review verdict or sending a branch back to an implementer; any number,
  measurement or factual claim to the owner; any plan, ordering or scheduling
  decision; any infrastructure or cost recommendation; and any retraction or
  correction. On 2026-09-29 every consultation made a material correction;
  the three unconsulted decisions — gate-box reprovisioning, the #222
  send-back, and a CI lock diagnosis — were each wrong or self-serving.
  On #222 codex found the relayed fix incomplete: a second `EXEC_START` sets
  `active_row = -1`, so plain `CMD_END` cannot close the orphan row. Fable
  found the implementer had already written the fix, the claimed ~100 zombie
  rows were ~0 in the demo workload (the real hazard was one presenter
  cancel), and a parallel branch overlapped on `tests/mock_server.py` and
  `tests/test_web_ui.py`. Neither adviser alone caught all of it. Cost is no
  reason to skip consultation: dispatch both in parallel, do independent work
  while they run, and block only work that depends on their answers. A wasted
  implementer round trip, owner-visible wrong statement or burned counted
  rehearsal attempt costs more. The adviser reviews reasoning, not code, and
  never edits, commits or creates cloud resources. Its verdict is advice,
  not authority: the lead may overrule it, but then says so in the report.

## Rules

- Branch `agent/<slug>`; one task per branch; work in an isolated git worktree
  (`EnterWorktree`). Two agents in one checkout collided once — never again.
  An implementer's worktree **stays until its PR is open**: the reviewer reads
  `tests/results/` evidence out of it, and removing it also makes the agent
  unresumable (2026-09-25: deleting one early lost #119's box logs and
  orphaned a VM).
- A reviewer **does not re-run** `make box-check` / `make ui-gallery` when the
  branch's evidence exists and is newer than the last code commit. Re-run only
  when it is missing, stale, or the log contradicts what the implementer
  claimed — and say in the PR body which applied.
- **One throwaway VM per agent, reused across rounds** (`KEEP=1`), deleted by
  that agent when it finishes, with the delete output pasted in its report.
  Never one VM per iteration; never leave one running for the 6h sweep to
  collect. **Delete it by id, never by sweep cutoff**: `tests/hetzner-vm.sh
  delete <id>` (it confirms via the API that the machine is actually gone
  before returning) — not `MAX_AGE_HOURS=0 make hetzner-sweep` or any other
  cutoff small enough to only match your own VM. A low cutoff matches every
  `pgwt=ephemeral` VM regardless of owner, including another agent's
  in-flight box-check (issue #162); `tests/hetzner-sweep.sh` now refuses a
  cutoff below 1h outright (`--force-all` to override deliberately) and
  never deletes a VM younger than a few minutes, or one whose age can't be
  determined at all, even then — but the sweep is a janitor for *stale* VMs,
  never the tool for removing one specific machine.
- Never harden a test against runner noise (retries, wider tolerances,
  confirmation loops). If it is red only on shared runners, say so in the PR;
  it belongs on the dedicated gate box.
- `tests/run_all.sh`'s `KNOWN_FAILING` table (test name → tracking issue
  number) is only for a test that reproduces a real, filed product bug being
  fixed in `src/`; it never covers timing or runner noise, which gets moved
  or investigated, not listed. A listed test still runs every time; neither
  outcome fails the gate — an expected failure and an unexpected pass
  ("UNEXPECTED PASS ... intermittent or fixed; check the issue") both count
  as `known-failing`, the pass also as `xpass`. The reviewer checks any
  `xpass` line against its issue rather than assuming the list is stale. A
  listed test that could not even run (exit 126: not executable, 127:
  command/file not found) still fails the gate regardless of
  `KNOWN_FAILING` membership — that is not the known product bug, it is a
  broken test invocation.
- Never commit locally generated snapshot PNGs to `tests/web_snapshots/`
  (font rendering differs from CI). Baselines are regenerated by the CI
  `snapshots` job only.
- Unit tests: add C tests to `tests/unit_tests.list` (single source of truth);
  UI logic goes in pure builders with a `tests/web_unit/*.test.mjs` case.
- `[skip ci]` only for docs-only commits.
- Do not push without a passing `make check` stamp (the hook will block you;
  do not bypass it — `PGWT_SKIP_PUSH_GUARD` is for humans). Likewise
  `make check` itself queues for a machine-wide lock so concurrent agents on
  this Mac serialize instead of colliding (`scripts/check-lock.sh`) —
  `PGWT_SKIP_CHECK_LOCK=1` bypasses it and is for humans only, same rule.

## Local setup (one-time)

Activate the Git pre-push stamp guard in each clone (this setting is per-clone
and covers its linked worktrees; `make check` also activates it when unset).
Setting `core.hooksPath` bypasses every hook in `.git/hooks/`: back up
or move any existing personal hooks into `scripts/git-hooks/` first.

```
git config core.hooksPath scripts/git-hooks
```

```
brew install node go
python3 -m pip install --user playwright==1.60.0 websockets pillow numpy
python3 -m playwright install chromium
export PGWT_BOX=root@<gate-box-ip>        # optional: PGWT_BOX_EL8, PGWT_BOX_EL9
```

`.python-version` (the pyenv pin with Playwright) is untracked (`.git/info/exclude`), so a new worktree only gets it via pyenv's global default or an explicit `PYENV_VERSION`, never by checking it out.

## Layout

- `src/` daemon + BPF (`src/bpf/`), `src/server.c` = `pgwt-server`
- `web/` Go bridge; `web/static/` UI — `views/` per-tab, `dev/gallery.html` fixture gallery
- `tests/` everything: `run_all.sh` (Linux master runner), `ci_smoke.sh` (real-PG
  capture), `test_web_ui*.py` (Playwright), `web_unit/` (Node), `web_snapshots/` (baselines)
- `docs/VISUAL_CHECKLIST.md` — the seven words a UI reviewer grades against
- `.github/workflows/ci.yml` gating; `nightly.yml` OS matrix
