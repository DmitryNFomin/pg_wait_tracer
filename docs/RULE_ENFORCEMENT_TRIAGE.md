# Rule enforcement triage

One row per rule in `CLAUDE.md` at `origin/master` (commit `88d161c6`, 301
lines, post-#707 shrink plus the owner's 2026-10-28 Models rewrite). Re-derived
independently from the file text; the two scratchpad artifacts cited in the
brief (`rule-inventory.md`, `unenforced-rules.md`) were built against an
earlier intermediate version and are used here only as a cross-check, not as
the source of row content or line numbers.

**Method.** Mechanical pass with the same modal/negative grep the prior
inventory used (`must|never|always|do not|only|refuse|required|before every`),
plus a manual second read for imperatives that grep misses (branching,
activation, command tables, role descriptions). Each row's "Existing check"
column was verified by `grep`/`git show`/`git ls-tree` against `scripts/`,
`.github/workflows/`, `tests/`, `Makefile` and `.claude/settings.json` on
`origin/master` this session, not carried over from the prior inventory
unchecked. Where a script exists only in an *open, unmerged* PR (#716), that
is stated explicitly and it is **not** counted as an existing check.

## Taxonomy reminder

| bucket | mechanism |
|---|---|
| (i) tool call | `PreToolUse` hook — deny, or inject `additionalContext` |
| (ii) turn boundary | `Stop` / `SubagentStop` hook |
| (iii) repo state | script in `make check` / pre-push |
| (iv) nothing observable | one advisory line, or delete |

## Full table

| id | rule (short) | CLAUDE.md line | bucket | proposed mechanism | existing check if any | counter that would show a violation |
|---|---|---|---|---|---|---|
| 1 | Audit every status report/progress summary/work-state answer before owner sees it | 5-6 | ii | Stop hook scans the turn's final assistant text for report-shaped language (five-line owner summary, 'merged/in flight/blocked') and requires a VERDICT line logged this session | scripts/lead/audit-report.sh exists; no hook gates its invocation | none (no log of audit runs vs reports sent) |
| 2 | Run scripts/lead/audit-report.sh <draft-file> before any status report | 8-9 | ii | same hook as #1; or SendMessage/report PreToolUse requiring a prior Bash call matching this script this turn | audit-report.sh exists, un-gated | none |
| 3 | Fix BLOCK drafts before sending | 12 | ii | same hook requires the LATEST audit-report.sh run this turn returned exit 0 (SEND) | audit-report.sh itself fails closed (exit 1/2) on BLOCK/malformed, but nothing stops the draft being sent anyway | none |
| 4 | Paste the audit verdict line with the report | 12 | ii | report-format validator (same hook) greps for 'VERDICT: SEND' in the emitted text | none | none |
| 5 | Run scripts/lead/held.sh for held claims; paste this turn's output | 13-14 | ii | report validator requiring a 'HELD/FREE' line sourced from a held.sh Bash call this turn | held.sh exists (sanctioned source), un-gated | none |
| 6 | Use only RUNNING/HELD/READY-NOT-STARTED vocabulary and fields | 16-18 | ii | status-schema regex over report text | none | none |
| 7 | FREE + not running => READY-NOT-STARTED by my choice, never 'blocked' | 20-22 | ii | report-text check: ban 'blocked' unless paired with a HELD citation this turn | sweep.sh [6] already computes READY-NOT-STARTED issues every refresh | sweep.sh [6] FINDING count (live, computed each Stop when cache >10m old) |
| 8 | Every report names the top non-running ledger item, with held.sh output or 'my choice' | 21-22 | ii | report validator cross-checked against sweep.sh [6]'s top item | sweep.sh [6] FINDING is already surfaced and gated by sweep-hook.sh's blocking disposition requirement, though not tied to report TEXT specifically, the underlying ledger fact is already forced into view every Stop | sweep.sh [6] FINDING count, closest thing to an already-collected counter in RULE 0 |
| 9 | Paste command+output for every count claim | 25-26 | ii | same audit mechanism as #1-4 | audit-report.sh's codex prompt explicitly re-derives counts (non-deterministic LLM judgment, not a grep) | none persisted |
| 10 | Paste command+output for every absence claim | 25-26 | ii | same | audit-report.sh prompt covers it; #716 (open, unmerged) adds absence-prepass.py specifically for this | none persisted on master; #716 would add one once merged |
| 11 | Paste command+output for permissions claims ('I can't') | 26 | ii | same | audit-report.sh prompt covers it generically | none persisted |
| 12 | Paste command+output for running claims | 26 | ii | same | audit-report.sh prompt covers it; sweep.sh [7]/[8] independently compute box/codex liveness every refresh | sweep.sh [7]/[8] output |
| 13 | One denied operation = one failed attempt, not universal denial | 27 | iv | no tool-call signature distinguishes 'treated as universal' from 'treated as one instance' except the report text itself | audit-report.sh prompt names this exact failure mode explicitly | none |
| 14 | Write UNVERIFIED when evidence is absent; never supply an unverified cause | 28 | ii | report-text validator requiring UNVERIFIED token where no evidence block precedes a causal claim | audit-report.sh prompt covers attribution-to-external-cause | none |
| 15 | Send design/scope/sequencing/approach/option questions to both advisers first | 32-33 | iv | no tool call distinguishes 'this decision is a design question' from any other; would need transcript classification | none | none |
| 16 | Ask owner only when advisers disagree/lead disagrees with both/decision is owner's | 34-35 | iv | same judgment problem | none | none |
| 17 | Tell owner what both advisers said and where the lead disagrees | 36 | ii | report-format validator requiring two adviser-position fields whenever an owner question is asked | none | none |
| 18 | sol 6.1 coder may consult Astra directly, but adviser must be a separate session (no self-advise/self-review) | 37-39 | i | PreToolUse on Task/SendMessage rejecting an adviser call whose target session equals the calling session | none | none |
| 19 | Try first; on failure tell both advisers what was attempted and what happened | 42 | iv | ordering/content can't be verified from outside the conversation | none | none |
| 20 | Report failures to owner only after advisers, with attempts/outcomes/options/recommendation, never bare FYI | 43-44 | ii | report-text validator rejecting the banned bare-FYI shape | none | none |
| 21 | If both advisers find no way, state what X/Y/Z they considered and each cost | 45 | ii | report-text validator | none | none |
| 22 | Never ship known wrong numbers/divergent paths/mismatched bases/disagreeing views, even green | 50-51 | iv | 'known defect' is a judgment call; no test enumerates all unknown-unknowns | none generic | none |
| 23 | Fix correctly; disclosure/caveats do not substitute | 51 | iv | same | none | none |
| 24 | Only an owner-worded explicit exclusion recorded on the issue permits shipping a known defect | 52-53 | iii | merge gate could grep the linked issue thread for an 'exclusion:' quote attributed to the owner before allowing a merge carrying a disclosed defect | none today | none |
| 25 | Lead cannot grant or infer an exclusion, including from 'better than master' | 53 | iv | judgment | none | none |
| 26 | Treat inability to report matching numbers as a product defect | 54 | iv | cross-view diff is domain-specific; no generic check exists across all metric pairs | none generic (#316-318 fixed specific instances) | none |
| 27 | Two views of one quantity must agree; fix the disagreement, don't pick which wrong number to show | 55 | iii | per-feature aggregate-vs-raw crosscheck tests, not a generic gate | specific tests in tests/unit_tests.list (e.g. agg/raw crosscheck, closed by #316-318) | per-feature test pass/fail, not generic |
| 28 | Run nothing eBPF-related on macOS | 59 | iii | platform gate | scripts/check.sh never builds/runs src/bpf locally (per memory 'Mac check never builds C'); only scripts/box-check.sh touches eBPF, and it SSHes to Linux | none explicit, but no Mac code path reaches it |
| 29 | make check before every push; push guard enforces it | 65 (table) | iii | EXISTS | scripts/git-hooks/pre-push + scripts/hooks/push-guard.sh compare .pgwt-check.stamp to scripts/tree-hash.sh | already enforced, migration candidate |
| 30 | make box-check before every PR | 66 (table) | iii | PR-creation gate requiring a box-check log newer than the tip | none (manual reviewer judgment only) | none |
| 31 | OS=el8 against PGWT_BOX_EL8 for kernel/libbpf/layout changes | 66 (table) | iii | changed-file classifier requiring an el8 evidence log | none | none |
| 32 | EPHEMERAL=1 supports OS=ubuntu only | 67 (table) | iii | EXISTS | scripts/box-check.sh refuses EPHEMERAL=1 with non-ubuntu OS | already enforced, migration candidate |
| 33 | make ui-gallery before every PR touching web/static or snapshots | 68/107 | iii | PR-creation gate requiring a current gallery run | make ui-gallery itself exists and works; nothing checks it ran | none |
| 34 | Persistent boxes FIRST; throwaway VM only as an exception | 72 | iv | 'exception' is a judgment call over what counts as genuinely busy | none | none |
| 35 | Check both pgwt-gate/pgwt-gate-2 before creating any VM; use a free one | 72-73 | i | PreToolUse on the Hetzner-create command requiring a fresh fuser/uptime check this turn | sweep.sh [7] already computes box occupancy every refresh, but is informational only, not wired to VM creation | sweep.sh [7] output, not yet gating creation |
| 36 | EPHEMERAL=1 only when both boxes genuinely busy; report which box and why | 73-75 | i | same gate plus report validator | sweep.sh [7] gives the live occupancy signal this needs | sweep.sh [7] output |
| 37 | Sweep deletes pgwt=ephemeral VMs older than 6h | 81 | iii | EXISTS | tests/hetzner-sweep.sh + tests/test_hetzner_sweep.sh | already enforced, migration candidate |
| 38 | Sweep runs automatically at every box-check start | 81-82 | iii | EXISTS | scripts/box-check.sh invokes tests/hetzner-sweep.sh at startup | already enforced, migration candidate |
| 39 | Sweep never touches pgwt-gate/pgwt-gate-2 by name, regardless of label | 82-83 | iii | EXISTS | PROTECTED_NAMES check in tests/hetzner-sweep.sh | already enforced, migration candidate |
| 40 | Sweep refuses cutoffs below 1h unless FORCE_ALL=1 | 83 | iii | EXISTS | tests/hetzner-sweep.sh + test | already enforced, migration candidate |
| 41 | Sweep never deletes VMs younger than a few minutes or of unknown age, even with override | 84-85 | iii | EXISTS | tests/hetzner-sweep.sh + tests/test_hetzner_sweep.sh (covers unknown/protected age) | already enforced, migration candidate |
| 42 | Unknown age is never treated as infinitely old | 85-86 | iii | EXISTS | same | already enforced, migration candidate |
| 43 | Remove your own VM by id (tests/hetzner-vm.sh delete <id>), never by sweep cutoff | 86-87 | i | Bash PreToolUse denying a MAX_AGE_HOURS=<small>/other-cutoff command whose apparent purpose is removing one named VM | the correct tool exists and API-confirms deletion; nothing stops using a cutoff instead | none |
| 44 | Declare a PR ready only with ALL definition-of-done evidence in its body | 92 | ii | PR-body validator checking all five items present | none mechanical (reviewer judgment) | none |
| 45 | Start box-check FIRST; run make check while it runs | 94 | iv | 'which started first' needs tool-call timestamps a post-hoc report check could read, but nothing does today | none | none |
| 46 | Full make check (not --fast) must pass on the final tree | 96 | iii | stamp would need to record fast-vs-full mode | the stamp does not distinguish the two today (flagged in memory 'stamp not exit code', issue #577-adjacent) | none, known gap |
| 47 | box-check passed; paste run_all summary (~20 lines) incl. live-UI-smoke verdict | 97-98 | ii | PR-body validator requiring the summary block and ui_live/summary.json verdict | none | none |
| 48 | CI-infra-only substitution requires no src/test changes, real gh workflow run, disclosed in PR body | 99-100 | ii | changed-file + PR-body validator | none | none |
| 49 | Skip reruns only after a first pass, for docs/tests-only rounds touching no live-tier test | 101-103 | ii | review-round + changed-file validator | none | none |
| 50 | Never exempt src/, web/, run_all.sh, unit_tests.list, or live-tier tests from box-check; if unsure, run it | 103-104 | ii | changed-file gate requiring fresh evidence for these paths | none | none |
| 51 | CI runs full on every PR; merge gates unchanged | 105-106 | iii | EXISTS (repo/branch-protection config, external to this repo's own scripts) | .github/workflows/ci.yml + GitHub branch protection | already enforced by platform settings |
| 52 | If web/static or web_snapshots changed: run ui-gallery, list summary.json counts and every changed/added/removed cell with justification | 107-109 | ii | PR-body/gallery-summary cross-check | none | none |
| 53 | Fresh reviewer approval, or open questions listed under 'Design questions for the owner' | 111-112 | ii | review-state gate | review chain is agent-performed, not mechanically gated | none |
| 54 | Give the owner only those design questions to resolve | 113 | iv | judgment | none | none |
| 55 | Use /pr-ready for the definition-of-done sequence | 115 | iii | entry-point gate requiring a recorded /pr-ready invocation | .claude/skills/pr-ready/SKILL.md exists (prose only, no invocation receipt) | none |
| 56 | Main session writes no feature code bigger than a one-liner | 119-120 | iv | diff-size heuristic tied to 'whose session wrote this', no such attribution exists | none | none |
| 57 | Model pins live in .claude/agents/*.md, overridden at spawn time | 122 | i | Task-tool PreToolUse comparing requested model to the agent file's pin and any authorized override | .claude/agents/*.md frontmatter declares the pin; nothing verifies the spawn matched it | none |
| 58 | Use the cheapest model that does good work; spend on review, economise on implementation | 123 | iv | quality/cost judgment, no proxy metric exists | none | none |
| 59 | codex writes the code via 'codex exec' with no -m flag; every PR gets a NEW codex agent, never resumed | 125-128 | i | PreToolUse on the codex invocation checking absence of -m and no prior session id reuse for this issue/PR | none | none |
| 60 | Claude agents review and challenge code; never the agent that wrote it | 129 | i | reviewer-launch gate comparing reviewer session identity to the author's | none | none |
| 61 | Astra writes designs via 'codex exec -m gpt-6-astra'; Fable challenges design decisions | 130-131 | iv | 'is this a design decision' classification | none | none |
| 62 | Any doubt, uncertainty or weird result MUST go to both advisers, not only when it seems worth it | 132-134 | iv | can't detect 'doubt' from tool calls | none | none |
| 63 | reviewer = Sonnet high effort, Opus for src/ logic, never downgraded | 135-137 | i | Task-tool gate checking model param against whether changed files include src/ | .claude/agents/reviewer.md frontmatter pin (declarative only) | none |
| 64 | ui-reviewer = Sonnet | 137 | i | same kind of gate | .claude/agents/ui-reviewer.md frontmatter | none |
| 65 | Explore/triage = Sonnet or Haiku | 137 | i | same | .claude/agents/*.md frontmatter | none |
| 66 | ONE review round: ship READY-with-nits with nits listed; second round only for a code blocker | 138-139 | ii | review-history validator requiring a recorded blocker before round 2 | none | none |
| 67 | Wait for box runs with one run_in_background wait, then read result; do not loop | 139-140 | i | a PreToolUse policy counting consecutive polling-shaped Bash/BashOutput calls and denying the Nth | none mechanical (memory notes self-matching pgrep/never-pkill incidents, but no hook exists) | none, candidate: deny the 3rd+ consecutive poll-shaped call |
| 68 | Reviewers stop agents that poll | 140 | iv | meta-rule about a reviewer's handling of another agent; no direct tool signature | none | none |
| 69 | Agent reports at most ~15 lines | 141 | ii | SubagentStop hook word-counts the final report text and flags overflow | none | none |
| 70 | Fable is standing adviser, not implementer, except explicit src/ escalation after a failed codex round | 143-145 | i | Task-tool gate checking agent type against an explicit escalation flag plus evidence of a prior failed round | none | none |
| 71 | Park and report non-escalated Fable tasks; never silently retry or downgrade | 145-146 | iv | requires judgment over session history | none | none |
| 72 | Record parked tasks in memory fable-parked-tasks | 146-147 | i | memory-write validator requiring this key exists when a park event is declared | none | none |
| 73 | Check the Fable budget before claiming 'out of tokens' | 147 | ii | report-text validator requiring evidence before that phrase is used | none | none |
| 74 | Main session model: Opus via /model, never Fable | 149 | i | session-start / /model-change gate pinning the orchestrator model | none | none |
| 75 | Main reads 15-line reports, decides ready/back, arms auto-merge and spawns | 149-150 | iv | role description, not a checkable negative | none | none |
| 76 | Minimise turns: act on reports, check once at a report-by deadline, never on 'still running' | 150-152 | iv | requires knowing WHY a turn was taken, not observable from a tool call alone | none | none |
| 77 | Do not read a diff when the reviewer's report already answers the question | 152-153 | i | PreToolUse on Read/git-diff denying it when a reviewer report covering that file already exists this session | none | none |
| 78 | Return signal-handling/loop/accounting/fail-safe findings to the implementer regardless of reviewer label | 153-154 | iv | requires semantic classification of finding content | none | none |
| 79 | Delegate web search/gh api reads/price lookups/code archaeology to Haiku/Sonnet, get one short report | 155-156 | i | PreToolUse denying WebSearch/gh-api-read tools to the orchestrator's own session, requiring a Task delegation instead | none | none |
| 80 | The orchestrator does not research | 156 | i | same as #79 | none | none |
| 81 | Capacity check every turn before replying to an agent report: implementer cap, codex, both boxes, advisers, BEHIND PRs | 158-161 | ii | SubagentStop/Stop hook requiring the turn's reply to reference sweep.sh-derived facts for each item | sweep.sh [3] (BEHIND+ARMED), [7] (box occupancy), [8] (codex liveness) already compute 3 of 5 items every refresh; implementer-count has no ledger on master (an agent-ledger.py exists only in the OPEN #716) | sweep.sh [3]/[7]/[8] output, near-migration for those three once a report-text cross-check is added |
| 82 | Answer the report AND fill idle capacity in the same turn | 161-162 | iv | same difficulty as #76 | none | none |
| 83 | Do not count blocked work as idle; blocking = shared files, not phase number | 162-164 | iv | requires file-diff reasoning per in-flight task | none | none |
| 84 | A box excluded from general gating remains usable to reproduce the issue that excludes it | 164-165 | iv | advisory/strategic framing, no violation signature | none | none |
| 85 | Owner reports at most FIVE LINES: merged/in flight/blocked/needs you | 167-168 | ii | report-text line-count + keyword validator | none | none |
| 86 | Longer form only for owner decisions or plan-changing findings | 168-169 | iv | judgment | none | none |
| 87 | Bug narratives go in the issue tracker; link them, don't retell them | 169 | ii | report-text validator flagging long inline narrative without an issue link | none | none |
| 88 | One roadmap item = one issue = one branch = one implementer | 171 | i | spawn-time registry checking the 1:1 mapping before a Task launch | none (no registry on master) | none |
| 89 | Split only along disjoint files AND disjoint tests; never split a shared file | 171-172 | i | pre-spawn file-overlap check across all in-flight branches | none mechanical today (manual 'before spawning' step only) | none |
| 90 | Cap actively-implementing agents at three, excluding idle CI/box/review waiters | 173-174 | i | Task-tool gate counting non-idle agent sessions against the cap | none on master; an agent-ledger.py exists only in the OPEN, unmerged #716 | none yet, near-migration once #716 merges |
| 91 | scripts/check-lock.sh serializes Mac make check machine-wide | 174-175 | iii | EXISTS | scripts/check-lock.sh/.py + tests/test_check_lock.sh (run directly at Makefile:331); check.sh itself wraps through check-lock.sh at Makefile:332 | already enforced, migration candidate |
| 92 | Apply no Mac-derived gate-phase cap; never invent a 'two in gate phase' limit | 175-176 | iv | this is a correction against a past wrong inference, not a positive obligation to check | none | none |
| 93 | Before spawning: list touched files, compare every in-flight branch, refuse overlap up front | 177-178 | i | pre-spawn file-reservation check (same mechanism as #89) | none mechanical | none |
| 94 | At most ONE branch touching web/static or tests/web_snapshots at a time; Go-only web/ changes exempt | 180-181 | i | spawn gate scanning open agent/* branches' changed files for the exclusive path set | none | none |
| 95 | Work over ~a day goes to a Plan agent first; steps run sequentially unless independence is shown | 181-182 | iv | task-size estimate is a judgment call | none | none |
| 96 | Always spawn with isolation: "worktree" | 184 | i | Task-tool param validator | none (trusted, not verified) | none |
| 97 | Spawn contract includes issue text, acceptance criteria, required make targets, stop-and-report, and report-by deadline | 184-186 | i | spawn-prompt field validator | none | none |
| 98 | Treat an absent notification as the hang signal; check once after the deadline, do not watch the stream | 186-188 | i | the same polling-detector as #67 applied to the orchestrator's own waits | none | none |
| 99 | For a file-backed contract, pass only its path; never also paste the text | 188-189 | i | spawn-prompt length/redundancy check | none | none |
| 100 | Review chain: scripts produce binding evidence; fresh reviewer writes PR body; blockers via SendMessage; main decides from the report, not the diff | 191-194 | i/ii | composite of #77, #104, #106 | partially (registration guards exist; invocation and diff-avoidance are not gated) | see #104/#106 |
| 101 | Only 'Design questions for the owner' reach the human | 194 | iv | judgment | none | none |
| 102 | On READY, arm auto-merge in the same turn; do not watch CI or merge by hand | 195-196 | i | PreToolUse denying a manual 'gh pr merge' once READY is declared this session, requiring 'gh pr merge --auto' instead | none | none |
| 103 | Do not push after READY | 196 | i | push-guard extension checking the target branch's PR review/merge state before allowing a push | scripts/hooks/push-guard.sh checks only the check-stamp, not READY state | none |
| 104 | Reviewer runs scripts/check-test-registration.sh before declaring READY | 199-202 | iii | READY-gate requiring a fresh guard run logged this review session | scripts/check-test-registration.sh EXISTS and is self-tested (tests/test_check_test_registration.sh); only the pre-READY invocation is unenforced | none |
| 105 | Match scripts/box-check.sh log commit=/tree-hash= against the PR head before trusting the log | 203-205 | iii | automated comparator run at READY time | NONE on master today; this exact mechanism (box-evidence-bulk.py / check-box-evidence.sh) exists only in the OPEN, unmerged #716 | none yet, would come from #716 |
| 106 | Reviewer runs scripts/check-runner-registration.sh manually against this repo before READY | 206-208 | iii | READY-gate requiring a fresh run logged this session | script EXISTS (scripts/check-runner-registration.sh/.py); make check runs only its synthetic self-test, not the live check | none |
| 107 | Leave live guard execution as a manual reviewer step; a human judges deliberate manual-only exemptions | 217-218 | iv | explicitly manual by design | n/a by design | n/a |
| 108 | Consult both advisers by default: Fable (standing second) and Astra | 220-221 | iv | can't tell from tool calls alone whether 'a decision' occurred requiring consult | none | none |
| 109 | Keep one Fable conversation per session; resume with SendMessage, never respawn | 221-223 | i | SendMessage/Task gate rejecting a duplicate adviser-session creation this session | none | none |
| 110 | Consult on every found issue/plan/numeric claim before acting and before reporting | 223-224 | iv | same classification problem as #108 | none | none |
| 111 | Consult before accepting/rejecting a review verdict or returning a branch | 224-225 | iv | same | none | none |
| 112 | Consult before any owner-facing number, measurement or factual claim | 225-226 | iv | same | none | none |
| 113 | Consult before plans, ordering or scheduling decisions | 226 | iv | same | none | none |
| 114 | Consult before infrastructure or cost recommendations | 226-227 | iv | same | none | none |
| 115 | Consult before retractions or corrections | 227 | iv | same | none | none |
| 116 | Never skip consultation for cost; dispatch in parallel, block only dependent work | 227-228 | iv | same | none | none |
| 117 | Advisers review reasoning, not code; never edit, commit or create cloud resources | 229 | i | PreToolUse denying Edit/Write/git-commit/Hetzner-create whenever the calling session's role is adviser | adviser agent contracts likely omit those tools by convention, not by a hook | none |
| 118 | Lead may overrule advice but must say so in the report | 230 | ii | report-text validator requiring an overrule disclosure whenever a decision contradicts a logged adviser reply | none | none |
| 119 | Branch agent/<slug>; one task per branch; isolated git worktree (EnterWorktree) | 235-236 | i | spawn-time branch-name/worktree-isolation validator | none | none |
| 120 | Keep the implementer's worktree until its PR is open | 236 | i | tool gate denying 'git worktree remove' before a PR exists for that branch | none | none |
| 121 | Reviewer reuses box-check/ui-gallery evidence newer than the last code commit; rerun only if missing/stale/contradictory, state which | 238-240 | iii | timestamp comparator run at review time | none on master (manual only); #716 (open) adds the bulk version of this | none yet |
| 122 | Reuse one throwaway VM per agent across rounds (KEEP=1); delete it when finished, paste the deletion output | 241-243 | i | VM-lifecycle tracker keyed by agent session | tests/hetzner-vm.sh provides the correct deletion primitive; no ledger ties a VM to an agent | none |
| 123 | Never create one VM per iteration, or leave one for the 6h sweep to collect | 243 | i | same tracker as #122 | none | none |
| 124 | Delete by id only; never MAX_AGE_HOURS=0 or any cutoff to remove one specific VM | 243-246 | i | same as #43 | none | none |
| 125 | hetzner-sweep.sh refuses cutoffs below 1h (--force-all overrides); even then never young/unknown-age VMs | 246-248 | iii | EXISTS (duplicate of #40-42) | tests/hetzner-sweep.sh + tests/test_hetzner_sweep.sh | already enforced, migration candidate |
| 126 | Never harden a test against runner noise (retries, wider tolerances, confirmation loops) | 250-252 | iv | can't mechanically distinguish 'noise hardening' from a legitimate tolerance fix | none | none |
| 127 | KNOWN_FAILING only for a real, filed src/ product bug, never timing/runner noise | 253-255 | iii | entry validator checking the referenced issue exists, is open, and is labeled as a real bug | tests/run_all.sh mechanically classifies outcomes against the table but does not validate entry legitimacy | none |
| 128 | Listed tests still run every time; expected failures and UNEXPECTED PASS both count known-failing without failing the gate; passes also count xpass | 255-257 | iii | EXISTS | tests/run_all.sh run_test() | already enforced, migration candidate |
| 129 | Reviewer checks every xpass against its tracking issue; never assumes the list is stale | 257-258 | ii | review-step validator requiring a disposition per xpass line | tests/run_all.sh prints the xpass lines (mechanism exists); nothing forces the reviewer to act on them | run_all.sh xpass count |
| 130 | Exit 126/127 fails the gate even for a listed KNOWN_FAILING test | 258-259 | iii | EXISTS | tests/run_all.sh explicit pre-check before known-failing classification | already enforced, migration candidate |
| 131 | Never commit locally generated PNGs to tests/web_snapshots/; baselines regenerate only via CI's snapshots job | 261-262 | iii | provenance check comparing file digest/commit author against a CI-run artifact | scripts/check-snapshot-history.sh checks history continuity, not true CI origin | none full |
| 132 | Add C unit tests to tests/unit_tests.list (single source of truth) | 264 | iii | EXISTS | tests/test_unit_list_coverage.py (registered at tests/unit_tests.list:214) | already enforced, migration candidate |
| 133 | Put UI logic in pure builders with a tests/web_unit/*.test.mjs case | 264-265 | iii | coverage mapper from each changed builder file to a matching .test.mjs | none generic | none |
| 134 | Use [skip ci] only for docs-only commits | 266 | i | commit/push hook rejecting a skip-ci marker when the commit touches non-doc files | none | none |
| 135 | Push only with a passing make check stamp; the hook blocks pushes without one | 267 | iii | EXISTS | scripts/git-hooks/pre-push + scripts/hooks/push-guard.sh | already enforced, migration candidate |
| 136 | Never bypass the push guard; PGWT_SKIP_PUSH_GUARD is for humans only | 268 | i | agent-environment policy denying that env var's use from an agent session | push-guard.sh honors the var for anyone by design, the 'humans only' half is a social contract, not a technical one | none |
| 137 | make check queues via check-lock.sh; never bypass PGWT_SKIP_CHECK_LOCK (humans only) | 268-270 | i | same kind of policy as #136 | check-lock.sh honors the var by design | none |
| 138 | Activate the pre-push stamp guard per clone (covers linked worktrees) | 275-276 | iii | EXISTS | scripts/check.sh auto-activates core.hooksPath if unset | already enforced, migration candidate |
| 139 | Before changing core.hooksPath, back up or move personal .git/hooks hooks to scripts/git-hooks | 276-277 | iii | preflight detecting personal hooks before the hooksPath change | scripts/check.sh warns about conflicting settings (partial) | none full |
| 140 | Install node and go with brew | 285 | iv | can't gate without actually running brew; make check fails downstream anyway if missing | scripts/check.sh fails if node/go absent (indirect) | none |
| 141 | Install playwright==1.60.0, websockets, pillow, numpy via pip | 286 | iii | preflight version check comparing installed Playwright version to 1.60.0 | scripts/check.sh checks importability only, not the exact version pin | none |
| 142 | Install Playwright chromium | 287 | iii | preflight browser-presence check | CI installs it in its own job; local scripts/check.sh does not verify it | none |
| 143 | export PGWT_BOX (optional EL8/EL9) | 288 | iii | EXISTS | scripts/box-check.sh resolves the variants and rejects a missing target outside EPHEMERAL mode | already enforced |
| 144 | .python-version is untracked; new worktrees get the pyenv pin only from global default or explicit PYENV_VERSION, never from checkout | 291-292 | iii | worktree-bootstrap check confirming a working interpreter is actually resolved | the untracked-ness itself is enforced by .git/info/exclude; nothing checks the worktree actually resolved a usable interpreter | partial |
| 145 | UI reviewer grades against the seven words in docs/VISUAL_CHECKLIST.md | 300 (Layout) | iv | reviewer-judgment artifact, not a hard pass/fail gate by nature | .claude/agents/ui-reviewer.md references the checklist; docs/VISUAL_CHECKLIST.md exists | none |

## Counts

| bucket | count |
|---|---|
| (i) tool call | 38 |
| (ii) turn boundary | 31 |
| (iii) repo state | 36 |
| (iv) nothing observable | 39 |
| composite (i/ii), row 100 only | 1 |
| **total rows** | **145** |

Reconciliation against the brief's "earlier inventory found 161 rules, 129
unenforced": that count was taken against the **pre-shrink, 423-line** file.
The current `origin/master` file is 301 lines and has been edited twice since
(the #707 shrink, then the owner's 2026-10-28 Models rewrite adding the
codex/Astra instructions). 145 is this file's own count, not a correction of
161, the two numbers describe different files. Of the 145, 38 bucket-iii rows
are marked EXISTS (already mechanically covered today); the remainder (all of
bucket iv, 39 rows, plus almost all of bucket i/ii, 69 rows) have no mechanical
coverage.

## Migration candidates (rule 3): counter already collected, baseline verified this session

A rule qualifies here only if an existing, already-wired mechanism computes
the compliance fact on every run, independent of whether an agent reads the
CLAUDE.md prose. Presence of every script below was verified this session by
`git ls-tree origin/master` / `git show origin/master:<path>`, not assumed
from the prior inventory:

| id(s) | rule | counter | baseline verified this session |
|---|---|---|---|
| 29, 135, 138 | push only with a passing stamp; activate the guard per clone | scripts/git-hooks/pre-push + scripts/hooks/push-guard.sh exit code on a stamp/tree-hash mismatch | both files present on origin/master; this branch's own make check run (below) exercises the stamp path |
| 32 | EPHEMERAL=1 supports OS=ubuntu only | scripts/box-check.sh exit code when invoked with EPHEMERAL=1 OS=el8 or el9 | script present; not re-invoked this session since this is a docs-only branch with no box-check requirement, so the baseline is "present and self-tested," not "re-run live today" |
| 37-42, 125 | hetzner-sweep protections (6h cutoff, protected names, 1h floor, unknown-age refusal) | tests/test_hetzner_sweep.sh pass/fail inside make check | registered at tests/unit_tests.list:311; runs every make check |
| 91 | check-lock.sh serializes Mac make check | tests/test_check_lock.sh pass/fail | not in tests/unit_tests.list; invoked directly at Makefile:331, and the lock itself wraps the real check.sh call at Makefile:332, two independent mechanical facts, not just a test |
| 128, 130 | run_all.sh known-failing/xpass/126-127 classification | tests/run_all.sh's own run_test() logic (Linux-only, exercised by box-check, not make check) | present on origin/master; not re-verified live this session, this branch is docs-only and does not run box-check (see Constraints) |
| 132 | C tests must be in tests/unit_tests.list | tests/test_unit_list_coverage.py pass/fail | registered at tests/unit_tests.list:214; runs every make check |

**What this means for CLAUDE.md, not acted on here (out of scope, this branch
does not edit CLAUDE.md):** these rows could become a single "(why: ...)"
pointer line each, the same pattern already used for every other rationale
link in the file, with the full prose moved to
docs/AGENT_WORKING_AGREEMENT_RATIONALE.md. Removing them would not weaken
enforcement, since the hook/script already runs unconditionally; it would
only shrink what is re-injected into every turn's context. No replacement
text is proposed here, since the brief scopes this task to triage, not to
CLAUDE.md edits.

**Rules with a nearly-collected counter, named for completeness, not proposed
for removal yet:** #81/#90 (capacity check, 3-agent cap). sweep.sh sections
[3]/[7]/[8] already compute BEHIND+ARMED, box occupancy and codex liveness on
every refresh, and the in-flight #716 adds agent-ledger.py for the implementer
count, but #716 is open and unmerged, so today there is no counter for
"agents actively implementing" and this stays a bucket-i proposal, not a
migration candidate.

## Top five single-mechanism wins (bucket i/iii, ranked by a cited incident, no invented frequencies)

| rank | id(s) | rule | mechanism | cited incident |
|---|---|---|---|---|
| 1 | 43, 124 | delete your own VM by id, never by sweep cutoff | Bash PreToolUse deny on a MAX_AGE_HOURS=<small>/low-cutoff hetzner-sweep invocation | issue #162; AGENT_WORKING_AGREEMENT_RATIONALE.md: "On 2026-09-28 thirteen throwaway VMs were created in a day," and a low cutoff "matches every pgwt=ephemeral VM ... including another agent's in-flight box-check" |
| 2 | 89, 93 | before spawning, refuse file-overlap across in-flight branches | pre-spawn script diffing each new task's planned files against every live agent/* branch | AGENT_WORKING_AGREEMENT_RATIONALE.md, "Shared file collision": 2026-09-25, "two branches both edited tests/test_durability.c, git merged them silently and the build broke" |
| 3 | 120 | keep the implementer's worktree until its PR is open | tool gate denying git worktree remove for a branch with no open PR | AGENT_WORKING_AGREEMENT_RATIONALE.md, "Worktree retention": 2026-09-25, "deleting one early lost #119's box logs and orphaned a VM" |
| 4 | 67, 98 | one run_in_background wait, then read; do not loop | PreToolUse deny on the 3rd+ consecutive poll-shaped BashOutput/status-check call in a short window | AGENT_WORKING_AGREEMENT_RATIONALE.md, "Polling cost": 2026-09-29, "one polling loop sent 15 such notifications in 2h10m, costing ~9M cache-read tokens (~5% of the day's spend)" |
| 5 | 103 | do not push after READY | push-guard extension checking the target branch's PR state before allowing a push | AGENT_WORKING_AGREEMENT_RATIONALE.md, "Auto merge and READY": 2026-09-28, "#228, #229 and #233 hand-merged within 13 seconds ... Required approving reviews is 0: a push after READY would merge unreviewed" |

No frequency numbers are invented above; each line cites the same incident the
rule's own rationale heading already cites. No dated incident was found for
the model-pin-verification rows (#57, #63-65) or the one-Fable-conversation
row (#109), so they are not ranked here even though they are plausible
bucket-i candidates, see Questions for the owner.

## Already covered, not proposed again

- **#690 (merged, blocking Stop/SubagentStop hook).** Mechanically live today
  for: #5 (held.sh is the sanctioned source), #7/#8 (sweep.sh [6]
  READY-NOT-STARTED computation, gated by the blocking disposition
  requirement), the BEHIND+ARMED half of #81 (sweep.sh [3]), and the box/codex
  liveness half of #81 and #12 (sweep.sh [7]/[8]). It does not cover #1-4, #6,
  #9-11, #14 (RULE 0's report-formatting and evidence-pasting rules): the hook
  gates FINDING lines from sweep.sh's own ledger, not whether a report the
  lead writes actually ran audit-report.sh or pasted its verdict.
- **#700 (merged, section-dispatch registration guard).** Not a CLAUDE.md-rule
  retirement, it is new infrastructure (every numbered C-test section must be
  reachable from main()) with no corresponding row in this table. It
  reinforces the purpose behind #132 (unit_tests.list as single source of
  truth) by making sure a registered suite's sections cannot be silently
  undispatched, but it does not change #132's own enforcement mechanism.
- **#716 (open, NOT merged, confirmed by git ls-tree origin/master showing
  none of scripts/lead/agent-ledger.py, box-evidence-bulk.py,
  check-box-evidence.sh, absence-prepass.py present).** Would, once merged,
  mechanize: #90 (3-agent cap, via agent-ledger.py's start/stop hooks), #10
  (absence-claim evidence, via absence-prepass.py wired into audit-report.sh),
  and #105/#121 (commit/tree-hash match and reviewer-evidence staleness, via
  box-evidence-bulk.py / check-box-evidence.sh, run in bulk across every open
  PR from a new sweep.sh section). These rows are marked "near-migration" /
  "none yet" above rather than EXISTS specifically because the PR is still
  open; they must not be proposed as new work here since #716 already is that
  work.

## Questions for the owner

1. **Dead rule?** #92 ("apply no Mac-derived gate-phase cap; never invent a
   two-in-gate limit") is a correction against a past wrong inference, not a
   positive, checkable obligation. No mechanism or counter could be
   constructed for it, and no case was found where omitting it caused a
   problem (as opposed to the original wrong inference it corrects). It was
   not dropped, per rule 1 of the brief; it stays bucket (iv) and is flagged
   here instead.
2. **#107** ("leave live guard execution as a manual reviewer step, let a
   human judge deliberate manual-only tools") is bucket (iv) by the rule's own
   design, not by a gap, it explicitly says this should stay manual. It is
   kept as advisory rather than given a mechanism, since proposing one would
   contradict the rule's stated intent.
3. **No incident found** for the model-pin-verification rows (#57, #63-65),
   the one-Fable-conversation row (#109), or the "orchestrator does not
   research" rows (#79-80). They are plausible bucket (i) candidates, but
   ranking them would mean inventing a frequency, which the brief forbids.

## Constraints followed

- Branch `agent/rule-enforcement-triage` off `origin/master`; docs-only diff.
- `CLAUDE.md`, `scripts/lead/*`, `.claude/settings.json` not edited.
- PR not opened.
