# Agent working agreement rationale

Historical rationale extracted from the pre-edit `CLAUDE.md`.
The excerpts retain their original wording, dates and numbers. Mixed passages
are quoted as historical context; every operative rule remains in `CLAUDE.md`.

Editorial test: "a rule without a script is a wish; prose goes in the issue tracker, not the always-loaded file" — applied here as: prose goes in the rationale doc, not the always-loaded file.

## Held claims

> Saying "blocked" there converts a scheduling
> decision into an external constraint, which is the failure that cost the owner
> eleven hours on the v9 epoch (files free from 14:19Z and 19:12Z; reported blocked
> in three consecutive reports).

## Advisers before owner

> Asking the owner to choose between options the advisers were never shown is a waste of
> his time and has been called out as such.

## Failure reports

> A bare caveat --
> "this does not work, FYI" -- converts the lead's unsolved problem into the owner's
> information, which is the same move as "blocked".

## Matching numbers

> Corollary, stated because it has been got wrong: **"we cannot report matching numbers" is
> itself a product defect**, not a presentation choice. Two views of one quantity must agree
> or the product is wrong. Do not ask which wrong number to display — fix the disagreement.

## Ubuntu VM evidence

> the same live tier as above, on a private one-run VM — no `$PGWT_BOX`, no flock contention with other agents | **every agent run on Ubuntu, including the final pre-PR one** — the throwaway VM is the same Hetzner machine class as the gate box (103b1a0 matched cpx42→cx33), so for the Ubuntu tier it is the same evidence. The two persistent boxes are reserved for CI's self-hosted jobs: an agent running there queues behind CI on the same `flock`, which is most of the 29–117 min CI spread (n=9 runs).

> because no `pgwt=gate-snapshot` image exists for el8/el9. So kernel/libbpf/layout work still runs `OS=el8` against `$PGWT_BOX_EL8`: 103b1a0 matched the machine class, never the kernel, and el8 is exactly the tier where that difference is the point

## Persistent boxes first

> **Persistent boxes FIRST; a throwaway VM is the exception** (owner rule
> 2026-09-28: *"use second gate box for next tests as soon as it's idle, do not
> create new machine, create a new ONLY if it's needed and gate box busy with CI
> or other tests"*). Both `pgwt-gate` and `pgwt-gate-2` are paid for whether or
> not they run anything — a stopped Hetzner server still bills, so an idle box is
> pure waste. Before creating any VM, check both boxes; use one if it is free.
> `EPHEMERAL=1` is legitimate only when **both** are genuinely busy (CI job or
> another agent's run holding the flock), and the report must say which box was
> busy and why. On 2026-09-28 thirteen throwaway VMs were created in a day, each
> re-provisioning PostgreSQL and pgbench from scratch, while both gate boxes sat
> idle for nine to thirteen hours — and the multi-minute provisioning window was
> itself the cause of three failed Mac-side attempts, because the display slept
> and the session locked while it ran. Using a ready box removes the cost and the
> failure mode together.

## VM cleanup

> `make hetzner-sweep` deletes any `pgwt=ephemeral`-labelled Hetzner VM older than
> 6h (also runs automatically at the start of every `box-check`); the
> persistent gate boxes (`pgwt-gate`, `pgwt-gate-2`) are never touched by it,
> by name, regardless of label. A cutoff below 1h is
> refused (`FORCE_ALL=1` to override) and a VM younger than a few minutes, or
> whose age can't be determined at all (missing/unparseable `created=` label —
> unknown age is never treated as "infinitely old"), is never swept regardless
> of cutoff (issue #162) — to remove your own VM, delete it by id
> (`tests/hetzner-vm.sh delete <id>`), never via the sweep's cutoff.

> A low cutoff matches every
>   `pgwt=ephemeral` VM regardless of owner, including another agent's
>   in-flight box-check (issue #162); `tests/hetzner-sweep.sh` now refuses a
>   cutoff below 1h outright (`--force-all` to override deliberately) and
>   never deletes a VM younger than a few minutes, or one whose age can't be
>   determined at all, even then — but the sweep is a janitor for *stale* VMs,
>   never the tool for removing one specific machine.

## Parallel checks

> They
>    use different machines and are independent (the check's flock is local to
>    this Mac, the box run holds its own remote flock, and `box-check.sh`
>    excludes `.pgwt-check.stamp` from its rsync), so running them in series
>    adds the check's whole wall time to every PR for nothing. Measured
>    2026-09-27 on this Mac under concurrent agent load: 6, 9 and 7 minutes
>    (n=3, median 7) — `scripts/check.sh`'s "~4 min" header describes an
>    unloaded run.

## Models and review

> (owner rules 2026-09-20 and 2026-09-25: cheapest model that does good work,
>   and **spend on review, economise on implementation** — a blocker caught in
>   review costs one message, one that reaches CI costs an hour of serialised
>   gate-box time):

## Polling cost

> this
>   harness blocks long standalone `sleep` commands, and each short backgrounded
>   wait emits a `stopped with background work` notification that makes the
>   orchestrator re-read its full context. On 2026-09-29 one polling loop sent
>   15 such notifications in 2h10m, costing ~9M cache-read tokens (~5% of the
>   day's spend).

## Fable history

> (owner rule 2026-09-25): on #128 it needed two
>   blocker rounds, still failed CI, cost more than every Sonnet agent of that
>   night combined, and died mid-task on its own usage limit; the main session
>   running on Fable cost more than all subagents together.

> Fable's budget was exhausted on 2026-09-25 and had **reset by 2026-09-27**,
>   since when it has been in continuous use as the adviser;

## Orchestrator turns

> and every one of its turns re-reads the whole context, so its model
>   is the largest single token cost.

> Each orchestrator tool call costs a full context re-read of a
>   conversation that only grows; on 2026-09-29, 57% of its turns followed its
>   own tool calls.

## Idle capacity

> (owner rule
>   2026-10-09: *"why I asking you every time find that there are idle resources and
>   waiting for it work? Any good reason?"* — there was none). The orchestrator is
>   event-driven by default: each report triggers a response about that report, and
>   nothing asks "what else could be running?", so idle capacity persists until the
>   owner notices. It happened three times on 2026-10-09 — one implementer against a
>   cap of three while two independent modules waited; codex idle twice with unblocked
>   work available; gate-2 at load 0.02 while gate-1 sat at 7.30 with its lock held.

## Owner report limit

> - **Reports to the owner are FIVE LINES at most** (owner rule 2026-09-25):
>   merged / in flight / blocked / needs you. Long form only when the owner has
>   to decide something, or when a finding changes the plan. No bug narratives —
>   the issue tracker holds the detail, and a link is enough.

## Splitting and gate capacity

> This is safe because `make check` on this Mac is now serialized machine-wide
>   by `scripts/check-lock.sh` (the `check:` Makefile target): a third agent
>   queues for the lock instead of piling a third concurrent Playwright/chaos
>   run onto the same laptop. But the lock is a CPU cap, not a queue that costs
>   nothing. Corrected 2026-10-08: the figure here used to read "3 x 22 min",
>   which contradicted the measured median at the top of this file. Three agents
>   in GATE phase are 3 x ~7 min (the n=3 median above), so **~21 min, under the
>   40-min box run — the BOXES are the critical path, not this Mac**, at every
>   agent count we actually run. So there is no Mac-derived cap on agents in gate
>   phase (gate phase = waiting on or running `make check`); the lock makes them
>   queue, and the queue drains faster than a box run. The cap that binds is the
>   three-actively-implementing one above, plus the one-`web/static/`-branch
>   rule. Do not re-derive a "two in gate phase" limit from this paragraph: it
>   came from the wrong number.

## Shared file collision

> (2026-09-25: two branches both
>   edited `tests/test_durability.c`, git merged them silently and the build
>   broke).

## UI branch scope

> (narrowed 2026-10-25,
>    same reasoning as the one-branch rule above):

> (narrowed 2026-10-25: Playwright
>   snapshots run against `tests/mock_server.py`, never the Go bridge, so a
>   Go-only change under `web/` cannot move a baseline and does not need this
>   rule): UI branches collide on snapshot baselines and
>   `tests/web_snapshots/VERSION` by construction, and each collision costs a
>   manual merge plus a regeneration.

## Spawn contracts

> On
>   2026-09-29 every agent notification was `completed`, and 26 of 40 were
>   `stopped with background work` — a live-and-polling signal. A hung agent
>   emits nothing; absence is the only hang signal.

> The duplicate brief stays in the orchestrator's context and is
>   re-read for the rest of the session.

## Auto merge and READY

> Repository
>   "Allow auto-merge" and branch protection "Require branches to be up to date
>   before merging" (`strict`) were enabled 2026-09-29. Strict matters: on
>   2026-09-28 #228, #229 and #233 hand-merged within 13 seconds, each having
>   passed CI against base `5c5e3c66`, none tested against the others. Strict
>   forces the second and third to update and re-run CI against the combined
>   tree, where a shared-file break goes red. Required approving reviews is 0:
>   a push after READY would merge unreviewed. The contract's no-push-after-READY
>   rule is now load-bearing.

## Evidence guard incidents

> (added
>   after silent failures: a rebase that dropped `tests/Makefile`'s
>   `test_exec_index` command line with the target still listed, a
>   `box-check` log carrying no commit id so "passed" could be true of a tree
>   that no longer exists, and `tests/run_all.sh` / `tests/ci_smoke.sh`
>   drifting into independent membership so a `box-check` green proved
>   nothing about a gate only `ci_smoke.sh` ran):

## Runner registration gaps

> wiring the live check into every `make check` would turn the
>     real, already-filed gaps it currently reports into a permanent red for
>     every unrelated branch: `test_capture_smoke.py` / `test_uprobe_fired.py`
>     (ci_smoke-only, tracked by #433, not fixed here since fixing them means
>     editing `tests/run_all.sh`), and orphans `test_check_test_registration.sh`,
>     `test_gesture_gate.py`, `test_startup_probe_degrade.py`,
>     `test_v9_open_cpu_wired.sh` (found by the orphan scan's first real run;
>     each may be a deliberate manual-only tool rather than a bug — a human
>     call, not this guard's). Self-tests: `tests/test_check_runner_registration.sh`.

## Adviser incidents

> (owner rules 2026-09-27 and 2026-09-29): consult BOTH by default:
>   codex via `codex exec` is primary; Fable via `.claude/agents/adviser.md`
>   is the standing second.

> The retro found the lead's own output is the
>   only artifact with no fresh reviewer — every wrong statement that reached
>   the owner started there.

> On 2026-09-29 every consultation made a material correction;
>   the three unconsulted decisions — gate-box reprovisioning, the #222
>   send-back, and a CI lock diagnosis — were each wrong or self-serving.
>   On #222 codex found the relayed fix incomplete: a second `EXEC_START` sets
>   `active_row = -1`, so plain `CMD_END` cannot close the orphan row. Fable
>   found the implementer had already written the fix, the claimed ~100 zombie
>   rows were ~0 in the demo workload (the real hazard was one presenter
>   cancel), and a parallel branch overlapped on `tests/mock_server.py` and
>   `tests/test_web_ui.py`. Neither adviser alone caught all of it. Cost is no
>   reason to skip consultation: dispatch both in parallel, do independent work
>   while they run, and block only work that depends on their answers. A wasted
>   implementer round trip, owner-visible wrong statement or burned counted
>   rehearsal attempt costs more.

## Worktree retention

> Two agents in one checkout collided once — never again.
>   An implementer's worktree **stays until its PR is open**: the reviewer reads
>   `tests/results/` evidence out of it, and removing it also makes the agent
>   unresumable (2026-09-25: deleting one early lost #119's box logs and
>   orphaned a VM).

## Known failing invocations

> A listed test still runs every time; neither
>   outcome fails the gate — an expected failure and an unexpected pass
>   ("UNEXPECTED PASS ... intermittent or fixed; check the issue") both count
>   as `known-failing`, the pass also as `xpass`. The reviewer checks any
>   `xpass` line against its issue rather than assuming the list is stale. A
>   listed test that could not even run (exit 126: not executable, 127:
>   command/file not found) still fails the gate regardless of
>   `KNOWN_FAILING` membership — that is not the known product bug, it is a
>   broken test invocation.

## Snapshot provenance

> (font rendering differs from CI). Baselines are regenerated by the CI
>   `snapshots` job only.

## Check serialization

> Likewise
>   `make check` itself queues for a machine-wide lock so concurrent agents on
>   this Mac serialize instead of colliding (`scripts/check-lock.sh`) —
>   `PGWT_SKIP_CHECK_LOCK=1` bypasses it and is for humans only, same rule.

## Hook setup

> Setting `core.hooksPath` bypasses every hook in `.git/hooks/`: back up
> or move any existing personal hooks into `scripts/git-hooks/` first.
