# How this work was organised

Written at the 2026-11-10 handover, as the companion to `docs/HANDOVER_2026-11-10.md`.

This describes **how the setup actually ran**: who did what, which script does
which job, what evidence each step produces, and where the model failed. It is
deliberately not a copy of the rules. The normative agreement is `CLAUDE.md`,
and the reasoning behind each rule is in
`docs/AGENT_WORKING_AGREEMENT_RATIONALE.md`; read those for *what is required*
and this for *how it was carried out, and what that cost*.

---

## 1. Who did what

**The owner.** Sets goals and judges results. His domain, and no one else's:
money, machines, release timing, and any decision to ship a known defect (RULE 2
exclusions, which must be in his own words on the issue). In practice -- this is
how we worked, not a rule written anywhere -- label changes that move the
tag-blocker count were also proposed to him and never applied by the lead. He reads five-line reports by default and long form only for a
decision or a plan-changing finding.

**The lead** -- the main interactive session. Plans, splits work, dispatches
agents, judges what comes back, publishes to the issue tracker, and reports to
the owner. **It writes no feature code beyond a one-liner**, which is the rule
that keeps it honest: it cannot quietly fix what it is supposed to be judging.
Its own output is gated like anyone else's -- no status report reaches the owner
without passing the audit in section 5.

**Implementers** -- `codex exec` with the repository's default model pin
(`~/.codex/config.toml`, `gpt-6.1-sol`). **A new agent with fresh context for
every PR**, never a resumed one, so that nothing carries over an assumption from
a previous round. They write the code and run the gates in their own clone.

**Reviewers** -- Claude agents (`.claude/agents/reviewer.md`, `ui-reviewer.md`),
Sonnet by default and **Opus for `src/` logic**, because that review is where a
silently-wrong number gets caught. A reviewer never reviews code it wrote, and
there is **one review round**: ship READY-with-nits with the nits listed, and
take a second round only for a code blocker. The reviewer writes the PR body --
not the implementer, and not the lead.

**Landing agents** -- Claude agents that take an implementer's patch, put it on
a branch off `origin/master`, prove the tree identity, run the Mac gate and
push. They exist because of the sandbox constraint in section 3, and they do not
open the PR.

**Advisers** -- two, consulted in parallel and before acting: **Fable**
(`.claude/agents/adviser.md`) and **Astra** (`codex exec -m gpt-6-astra`). They
review reasoning, never code they wrote, and they never edit a file, commit, or
create a cloud resource. One session never advises itself. The lead may overrule
an adviser but must say so in the report, with the evidence.

**Plan and Explore agents** for work over about a day, and for broad searches
whose conclusion is all that is needed.

The cap is **three actively implementing tasks**, excluding agents idling in CI,
a box run or review. On this Mac the real constraint is tighter: `make check`
serialises machine-wide, so three concurrent gate-bound tasks is the practical
ceiling.

---

## 2. The unit of work

One roadmap item = one issue = one branch = one implementer. Splitting is
allowed only across **disjoint files and disjoint tests**; a shared file is
never split, and `scripts/lead/held.sh` is the only sanctioned way to claim a
file is held. At most one branch may touch `web/static/` or
`tests/web_snapshots/` at a time.

A brief is a file, and the agent is given its path rather than its text. It
states the issue, the acceptance criteria, the required `make` targets, "stop
and report, do not open the PR", and a report-by deadline. Two rules about
briefs were learned the hard way: **pin measurements, never conclusions** (a
pinned classification once made three reviews echo one judgement), and **read
the issue's comments, not only its body**, because the scope decision usually
lives in a comment that contradicts the title.

---

## 3. The pipeline, as it actually ran

```
brief (a file)
  -> scripts/lead/codex-task.sh <brief> <branch>        # implementer, in its own clone
       -> code + gate runs + a report; the branch does NOT move
       -> SALVAGE_PATCH: <brief>-salvage.patch
  -> landing agent (Claude, fresh worktree off origin/master)
       -> apply patch, confirm scripts/tree-hash.sh matches the box log header,
          commit, full make check, confirm the stamp, push
          (the hash can only match if origin/master has not moved since the
           implementer's run and the untracked set is the same, so land promptly
           and do not add files to the worktree first)
  -> reviewer (Claude, fresh context, never the author)
       -> verify evidence, EXECUTE the branch's own negative controls, write the PR body
  -> lead: open the PR, arm auto-merge in the same turn as the READY verdict
```

**The constraint that shapes all of it:** the implementer's sandbox forbids
writes under `.git`, so `git add` cannot take its lock and **an implementer
cannot commit**. Every round that reaches the end exits `BRANCH_UNCHANGED`.
That is expected, not a failure. **As of PR #842** the wrapper writes the
clone's working tree out as a patch for the landing agent (seventeen lines added
and one changed); on `origin/master` it does not, so check which version you
have before relying on `SALVAGE_PATCH`.

Two more properties of `codex-task.sh` worth knowing before using it. First,
it fetches only `master` and bases the branch on that: a fresh clone starts the
branch from `origin/master` and **never** from `origin/<branch>`, and an
existing clone that already has the branch checks it out instead. The brief
cannot change that base, so a round that must continue a published branch has to
prime the clone beforehand, or tell the agent to fetch and reset to the branch
itself inside the clone. Second, a `nohup ... &` launcher returns immediately, so
a "completed" notification means the launcher returned and not that codex
finished -- check `ps -o pid,etime,command -ax | grep "codex exec --cd"`.

---

## 4. Gates and what makes a piece of evidence valid

| gate | where | what it proves |
|---|---|---|
| `make check` (`check-fast` for seconds) | this Mac | Node builders, Go bridge, `py_compile`, Playwright UI and chaos against `tests/mock_server.py` |
| `make box-check [OS=…] [PG=…]` | x86 Linux over ssh | build, C units, synthetic data, protocol drift, real-PG capture, live UI smoke |
| `make ui-gallery` | this Mac | every UI snapshot cell, for any `web/` change |

**Evidence binds to a tree, not to a branch or a commit message.**
`scripts/box-check.sh` writes `commit=<sha> tree-hash=<scripts/tree-hash.sh>` in
its log header, and that log is evidence for a PR head only if the head's
`scripts/tree-hash.sh` equals the logged `tree-hash`. That identity is what let
an implementer's box run be reused for a commit that did not exist when the run
happened -- and it is what exposed the same claim as false for a second branch
on the same day.

**`make check` reports through its stamp**, `.pgwt-check.stamp`:
`scripts/check.sh` removes the stamp when a test fails, so a stamp equal to
`scripts/tree-hash.sh` is the pass signal and the exit code is not. One hazard
found while writing this handover: the stamp is computed when the run finishes,
so if the tree changes mid-run the stamp describes a tree the run never tested.
Do not edit the tree while its check runs.

Three guards a reviewer runs by hand, all Mac-side and seconds long:
`scripts/check-test-registration.sh`, `scripts/check-runner-registration.sh`,
and the log-header identity check above. `scripts/check-box-evidence.sh <pr>`
reports when a PR's logged evidence has drifted from its tip.

**The boxes are the CI runners.** An agent's box run starves CI and stalls
merges, so check the flock first
(`ssh root@<ip> 'fuser -v /tmp/pgwt-box-check.lock'`) and use
`make box-check EPHEMERAL=1` only when the persistent boxes are busy. A box
excluded from general gating is not a general scratch box: `CLAUDE.md` line 179
keeps it available for reproducing the very issue that excludes it. Delete a
throwaway VM **by id** (`tests/hetzner-vm.sh delete <id>`), never by a sweep
cutoff. The Hetzner token lives in the macOS Keychain, service `hcloud`,
account `claude_token`, and is read inside a subshell and never printed.

---

## 5. Reporting, and the audit that gates it

Every status report, progress summary, or answer about why work is or is not
happening goes through `scripts/lead/audit-report.sh <draft>` before the owner
sees it. It runs a pre-pass (every absence claim needs a pasted command and its
output within eight lines, or an explicit `UNVERIFIED:` marker) and then a codex
pass that re-derives each claim independently and labels it VERIFIED, WRONG or
UNVERIFIABLE. **The printed verdict is authoritative**; a piped run's exit code
is not, and the last printed verdict is the one that counts.

It is not a formality. On 2026-11-10:

```
$ find scratchpad -maxdepth 2 -name "audit-*.out" -newermt "2026-11-10 00:00" | wc -l
      14
$ for f in <those files>; do grep -ohE "VERDICT:? ?(SEND|BLOCK|MALFORMED|INCONCLUSIVE|AMBIGUOUS)" "$f" | tail -1; done | sort | uniq -c
  10 BLOCK
   1 SEND
```

Three print no verdict line, because their run was killed or left empty. Two of
the ten BLOCKs listed eight and seven wrong claims in a single draft. Every wrong claim that day was a **derived comparison** -- a ratio, a
"same scale as", a fraction, a generalisation from one capture -- and never a
measured number. The practice that follows: publish the measurement and its
command and let the reader divide; hold an adviser's number to the same bar as
your own; and say "on this capture" until a second capture agrees.

Corrections are published, not quietly fixed: the original stays visible and the
correction states what was wrong and where the error came from. Five such
corrections went out on 2026-11-10 across four issues.

`scripts/lead/status.sh` renders the owner-facing skeleton so a report starts
from the required shape rather than from prose. It needs
`scripts/lead/.sweep-latest.txt` less than 600 seconds old, so run
`scripts/lead/sweep.sh` first or it will refuse.

---

## 6. The reconciler and the disposition loop

`scripts/lead/sweep.sh` joins GitHub, git and local state and prints findings:
branches that are empty shells, armed-but-BEHIND PRs, VMs alive, box occupancy,
codex liveness, evidence drift, and the top tag-blocker issues that have no open
PR and no live worktree. It writes `scripts/lead/.dispatch-queue`.

The `Stop` and `SubagentStop` hooks (`scripts/lead/sweep-hook.sh`) **refuse to
end a turn** until every finding carries a disposition recorded that turn in
`scripts/lead/.dispositions`:

- a plain finding takes `ACTED <command>`, `MY-CHOICE <reason>` or
  `UNKNOWN <what could not be determined>`;
- an escalated `issue-N` item takes only `RUNNING <agent-or-PR>`,
  `BLOCKED-BY <other-queue-id>`, or `HELD-BY <pr> <file>` with this turn's
  `held.sh` output.

The vocabulary is the point: it makes "blocked" a query result rather than a
feeling. If `held.sh` says FREE and nothing is running, the honest status is
`READY-NOT-STARTED by my choice -- I ran X instead`.

Two limits to know. The ledger file can print `STATUS UNKNOWN` when its agent
census disagrees with its corroborating signals, while the sweep's stdout
findings remain usable. And its **ranking can go stale**: it listed #808 as the
top not-started item all day, when PR #825 had already shipped three of that
issue's four acceptance criteria. A finding is a prompt to check, not a fact.

`scripts/lead/selftest.sh` and `scripts/lead/codex-task-selftest.sh` are the
regression tests for this machinery; run them after touching anything under
`scripts/lead/`, and **not while a dispatch is inside the wrapper** -- `sh`
reads a script by byte offset, so editing a running script makes the running
copy resume into shifted bytes.

---

## 7. Script inventory

| script | what it does |
|---|---|
| `scripts/lead/codex-task.sh` | dispatch an implementer into its own clone; salvages the round as a patch |
| `scripts/lead/audit-report.sh` (+ `audit-report.py`, `absence-prepass.py`) | re-derive every claim in a draft report; SEND or BLOCK |
| `scripts/lead/held.sh` | the only sanctioned source of a "held by #N" claim |
| `scripts/lead/sweep.sh` | the reconciler: GitHub + git + local state into findings and the dispatch queue |
| `scripts/lead/sweep-hook.sh` | the Stop / SubagentStop gate that demands a disposition per finding |
| `scripts/lead/agent-start-hook.sh`, `agent-stop-hook.sh` (+ `agent-ledger.py`) | maintain the shared active-agent ledger |
| `scripts/lead/status.sh` | render the owner-facing status skeleton |
| `scripts/lead/check-box-evidence.sh` (+ `box-evidence-bulk.py`) | report PRs whose box evidence has drifted from their tip |
| `scripts/lead/selftest.sh`, `codex-task-selftest.sh`, `audit-report-selftest.py`, `selftest-pr-state.py` | regression tests for all of the above |
| `scripts/box-check.sh` | the live tier over ssh; writes the `commit=`/`tree-hash=` header that makes a log citable |
| `scripts/check.sh` | the deterministic Mac tier; writes and removes `.pgwt-check.stamp` |
| `scripts/check-lock.sh` | machine-wide mutual exclusion around `make check` |
| `scripts/tree-hash.sh` | content hash of the working tree, the identity every piece of evidence is bound to |
| `scripts/pr-ready.sh` | the mechanical definition-of-done sequence, run from inside an agent worktree |
| `scripts/check-test-registration.sh`, `check-runner-registration.sh` | refuse a tree that silently drops or orphans a test |
| `scripts/check-link-closure.sh`, `check-runner-pin.sh`, `check-section-registration.sh`, `check-snapshot-history.sh` | the sub-guards `scripts/check.sh` calls on every `make check` |
| `scripts/hooks/kill-guard-selftest.sh` | the kill-guard self-test CI runs |
| `scripts/hooks/push-guard.sh` + `scripts/git-hooks/` | refuse a push without a stamp matching the tree; refuse attempts to bypass itself |
| `tests/hetzner-vm.sh`, `tests/hetzner-sweep.sh` | create and delete throwaway VMs by id; the sweep is a janitor, not a targeted delete |

---

## 8. Where this model failed, concretely

A new team should expect these, because they are properties of the setup rather
than one-off mistakes:

1. **The dispatch path could not produce a commit, and that went unnoticed for
   twenty rounds.** The sandbox's `.git` restriction was visible in every
   wrapper log as `BRANCH_UNCHANGED`; each round was hand-salvaged instead of
   the tool being fixed, and section 9 records that the two-stage shape is itself
   the defect rather than the remedy.
2. **The lead's prose was the least reliable artefact in the system.** The
   measurements held up; the sentences about them did not. Thirteen audit rounds
   in a day, eleven refused. The audit caught every instance, but it caught them
   downstream -- the same scrutiny applied to a brief before dispatch is
   cheaper.
3. **A reviewer can inherit a gap** -- seen once, so treat it as a warning
   rather than a property. A landing agent ran a branch's unit-level controls
   but not its standalone refusal controls, and reported exactly that; the
   reviewer then had to execute them. Name, in the review brief, which controls
   the reviewer must run itself.
4. **One session's scratchpad ends up load-bearing** -- also seen once, at this
   handover, but with nine artefacts at stake: they existed on one Mac only, and
   the handover itself cited a plan file that lived in a temp directory.
   Anything another person needs goes into the repository or an issue comment.
5. **Measurement evidence outgrows the places it can live.** This week's
   captures are about 1.1 GB under `/private/tmp`; the published numbers cite
   them. Decide, before measuring, where the capture will live if the number
   gets published.
6. **The reconciler's ranking ages.** See #808 in section 6.

## 9. What I would change

- Give the implementer a sanctioned way to commit, or make the landing step a
  first-class part of the wrapper rather than a second agent.
- Audit briefs, not just reports.
- Put the adviser consult and its verdict in the issue thread at the time, not
  in a session that disappears.
- Keep a single durable index of where every published number's evidence lives,
  updated when it is published rather than at a handover.
