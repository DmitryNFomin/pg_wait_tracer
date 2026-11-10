# Repository traps, carried over from the outgoing lead's working memory

**Provenance and standing: these are unverified working notes, not an
agreement.** They were written as one-line reminders by the lead that has just
handed over, each after an incident, and they were never audited the way a
report or an issue is. Several name machines, paths or scripts that may have
changed since. Treat every line as a hypothesis to check against the current
tree -- `CLAUDE.md` is the agreement, `docs/OPERATING_MODEL.md` describes how
the work actually ran, and this file is only the accumulated scar tissue.

They are copied here because they lived in one machine's agent memory
(113 entries) and would otherwise be invisible to anyone working elsewhere. The
selection below is the subset about **this repository and its tooling**; notes
about the outgoing lead's own habits and preferences were left out, and the
dozen that matter most are already written up properly in the handover's traps
section.

Anything that contradicts the current code is stale, and the code wins.

- **Gate box** — root@2.28.45.47, cx33, PG ports 541x, flock rule, noise numbers
- **Gate process decisions** — cx33 stays; intermittent product bugs may be KNOWN_FAILING; #110 miss does not block PRs
- **Orphaned background shells** — poll loops outliving agents; only /tasks or the human can clear them
- **Gate box CPU vendors** — same cx33, different silicon; owner declined the swap 2026-09-29
- **Verify before asserting** — 2026-09-30: errors were selective verification, not context; read the source in-turn
- **Codex subagent roles** — SUPERSEDED by codex-implements-claude-reviews.md; the split was backwards
- **Cross-validation numbers not comparable** — identical test sha hides a changed comparator (#308) and a 3M-row table (#312)
- **Auto-merge does not update a BEHIND branch** — armed PRs stall silently under strict; run gh pr update-branch
- **Push guard vs agent worktrees** — .claude/worktrees/ is untracked and moves the tree hash; guard unsatisfiable while agents run
- **Stale local master ref + stale main checkout** — local master AND the main working tree are both stale; read code only via git show origin/master:<path>
- **Gate boxes are the CI runners** — agent box-checks starve CI and block merges; use EPHEMERAL=1 while a queue drains
- **Codex invocation** — no -m means the DEFAULT gpt-6.1-sol; Astra is -m gpt-6-astra; stdin from /dev/null; bypass flag is refused here; exit 0 ≠ finished
- **Paint latency real cause** — it was the sort + per-row rescans, not compute; indexes were the wrong instrument
- **Paint measurement canary** — canary is transitions not time_model; measure warm in one process
- **Unlabelled VMs eat the quota** — invisible to the sweep; shows up as a VM-create failure, not a stray
- **Mac can run summary-vs-raw** — local pgwt-server builds, but build-server-local.sh needs -include demos/replay-compat.h added by hand (see the build-script issue)
- **Never pkill check patterns** — pattern hits every worktree; kill by PID after checking cwd, or wait for the queue
- **Scratch worktree tests the committed tree** — negative controls pass vacuously if the wiring is uncommitted; commit first, then grep the step name in the log
- **codex implements, Claude reviews** — owner 2026-10-18, repeated; codex writes code, Claude agents criticise it; net/keychain are the only exceptions
- **Never pin conclusions in briefs** — pin measurements only; a pinned classification made 3 reviews echo 1 judgement
- **make check log inside the tree** - an in-repo redirect makes the push stamp unsatisfiable by construction; write to scratchpad
- **Adviser handback not delivered** — completed with no text; extract assistant blocks from the tasks JSONL, never cat it
- **Inherited-defect rationalisation** — "already in master" fails when the branch creates the dependency; check for a false superlative, not just silence
- **Review premise first** — reviews verify the diff against a wrong premise; require reproduce-or-refute + read the untouched consumer
- **Durable before delete** — verify the archive at its FINAL path before deleting any remote copy; worktrees vanish mid-turn
- **Self-matching pgrep** — a wait loop whose pattern is in its own cmdline never exits; cost 2.5h once and 8 orphans another time
- **Implication guards go vacuous** — "A implies B" tests nothing where A is false; remove the possibility, don't add an assertion
- **Composite check names lie** — a red check's NAME is not its failing sub-check; unpack the artifact before naming a cause
- **Always video-analyse with both advisers** — UI failure or deliverable: record/find video, both advisers, before theorising
- **box-check rsyncs stale objects** — tests/*.o travels to the Linux box; a valid stale ELF would link silently
- **update-branch invalidates evidence** — the merge commit it creates has no check/box-check; disarm and re-run if it touched run_all.sh or src/
- **Resurrected worktrees are stale** — old agent-* worktrees produce no-op PRs; check merge-base date and diff files vs master first
- **Live walk needs a writing daemon** — --archived cannot make GIFs; first live walk succeeded 2026-10-25 but is not a counted one
- **Stamp, not exit code** — make check can report success while exiting 2; only the stamp vs tree-hash is sound (#577)
- **Verify every item before closing** — multi-item issues: the refuting evidence is often inside the same report you're acting on
- **Never cp -R a worktree** — the copy's git writes to the ORIGINAL worktree's index; use git worktree add
- **Unpushed branch is no fix** — three finished branches existed only on this Mac; ls-remote before planning
- **Mac check never builds C** — a C test passed twice locally while uncompilable; C needs a box-check
- **Check-lock queue is the cap** — no-Mac-cap holds at 3 agents, not 8; count waiters before assigning check-bound work
- **A proxy hides the name** — empty grep in a forwarding layer is not absence; read both sides of the boundary
- **Ask for the smallest shape first** — approve structure only after asking what invariant it needs; #612 took 4 rounds to get smaller than round 1
- **Derived metrics share numerators** — a substitute for a broken counter can restate the thing you correlate it against; read what the numerator excludes
- **Evidence belongs to a commit** — check a box-check log's commit= header against the tip under review; a neighbouring branch's green log reads like evidence
- **git show $ref:path returns a silent zero** — unquoted var + ugrep function = confident false negative in the mandated read pattern; quote it and use /usr/bin/grep
- **Broadened guard retires a gate** — a wider guard makes a narrower mutant equivalent; re-run mutants AFTER the fix
- **Squash merge inherits closing keywords** — "should this close #N" in a PR body CLOSES #N; check gh closingIssuesReferences, and read your grep output
- **Never say blocked, run held.sh** — blocked is a query result; FREE means READY-NOT-STARTED plus what I ran instead
- **Audit before send, not after** — RULE 0 means WAIT; the one time I sent early the verdict was BLOCK on nine claims
- **Check the lock HOLDER, not just the state** — an agent run on a gate box queues CI behind it; gate-3 DOES carry a runner (re-verified 2026-11-02), so probe all three
- **A gate that cannot FIRE** — 17/17 green and inert; every measured gate needs one control against real state, and an absurd value is a refusal
- **Audit deficiency claims too** — I reported three checks as missing that I had written myself; absence claims need the same evidence as progress claims
- **Never route a denied write to an agent** — refused in my session means it goes to the owner, not sideways; "it was already in its brief" is rationalisation
- **Empty var in a gh write hits another PR** — gh pr merge "" resolves to the current branch PR; guard the number before any write
- **Fork agents ignore narrow briefs** — a fork inherits full tools + context; one told only to relay filed a duplicate issue and faked its handback
- **codex implementer git sandbox** — codex builds in a /private/tmp clone; verify reachability and fetch, or the branch is lost
- **Squash merge breaks the whole stack** — merging a stack base squashed conflicts every child; redo the child's own files, don't rescue
- **A denial is not an owner task** — refused command means find another route or drop it; the owner's domain is money, machines, timing, defect acceptance
- **Updating all armed PRs starves the boxes** — strict means one merges; the other runs waste CI and hold the gate-box lock a box-check needs
- **Audit verdict is the printed line** — a piped run returns tail's exit 0; grepping only "VERDICT:" misses MALFORMED/INCONCLUSIVE/AMBIGUOUS
- **An issue citing a pre-merge sha** — two of three #714 items were already fixed; check is-ancestor before filing or dispatching
- **A stamp from a clone drifts** — info/exclude is per-repo; cat-file + diff-tree names the difference instead of ordering a re-gate
- **Merged is not running** — the main checkout goes stale; a just-merged gate fix keeps misbehaving until scripts/lead and hooks are refreshed
- **PRs 33/34 parked** — owner 2026-11-02; their src/server.c hold is not a constraint, check a PR is live before treating HELD as a block
- **Gate-snapshot label has no OS filter** — a new EL8 snapshot hijacks every EPHEMERAL=1 run; relabel, don't delete
- **nohup + background completes early** — the notification is the launcher; a finished log and a wedged log look identical
- **Adviser claims need provenance** — verified and inferred arrive in one voice; 2 of 4 were false and would have reached the owner
- **Counts and causes are where I overclaim** — delete the tally, split measurement from mechanism, attribute what you didn't run
- **Audit reads its own old verdicts** — stale audit files in the scratchpad fake an AMBIGUOUS; archive them, don't edit the draft
- **update-branch output reports a stale head** — re-read headRefOid in a separate call before pinning a sha into a brief
- **git stash is repo-wide** — a pop in one worktree can apply another agent's shelf; use git worktree add for absence controls
- **Never estimate a timestamp** — read date -u; a future-dated report is a provable falsehood and blocked an otherwise-clean draft
- **Backticks in a gh --body execute** — write bodies to a quoted heredoc + --body-file, then read the body back; inline ran make check on the real repo
- **Diff direction shows your own branch** — use old-master..new-master for 'what the merge added'; branch-tip..master invents overlaps
- **Closing refs miss commit bodies** — closingIssuesReferences is PR-body-only; scan commits before a squash merge
- **git cherry lies after a squash merge** — every commit reads unique; diff the aggregate against the squash commit
- **A lock taken per step is not held** — CI re-acquires the box flock each step; an agent slips into the gap and CI waits 46 min mid-job
- **"Unrelated failure" is often a held gate** — #115 gates every EL8 box-check; ask which issue and whether KNOWN_FAILING lists it
- **A dead agent orphans its VM** — salvage the results first (only copy), then delete by id; stamp and push do not survive
- **Require literal evidence paths** — "the scratch worktree" cost a search round; never read a filtered find or a head-piped list as absence
- **Merge queue costs more box time** — merge_group forces the FULL matrix per entry by design; repo is User-owned so it may not exist; fix is sequencing
- **An issue body is not the decision** — the owner scope decision lives in COMMENTS; use gh api .../issues/N/comments, --comments plus --json is rejected
- **Widen the conclusion, not the probe** — NOT-SEARCHED line + issue-comments precondition; moving the evidence gate earlier would pass all four errors; never pipe an existence probe to wc -l
- **The shell states the verdict** — never put a probe and its interpretation in one Bash call; derive the verdict from the data, never pre-write the label
- **An issue title's "deterministic" is a claim** — #804 was intermittent; a passing run I had already read refuted my queue-wide story
- **"does not close #N" still closes #N** — GitHub ignores the negation; I then published a false "manual link" cause
- **The cited section refutes the issue** — #573 asked for a format bump for data carried since v7; the refutation was in the test comment the issue named
- **Grep docs for the issue number** — I researched #439 for half a day; a decision doc already said no change was needed
- **Most gates run on Mac** — C unit tier + server-level Python gates build on macOS; only BPF, live PG and the daemon need a box
- **Box address from old logs** — agents ssh to recycled IPs mined from tests/results; resolve by Hetzner name, never from logs
- **Sweep has two disposition gates** — hash id takes MY-CHOICE/ACTED/UNKNOWN, issue-N takes RUNNING/BLOCKED-BY/HELD-BY; an escalated item needs both lines
- **codex-task.sh branches from master** — dispatching it on a published branch hands the implementer master; prime the clone and make the brief check HEAD
- **A stale checkout runs a superseded gate** — I filed a RULE 0 defect master had fixed 4 days earlier; check `git status -sb` for "behind" before trusting or blaming any gate
- **Never edit a running shell script** — sh reads by byte offset; an edit mid-run kills the wrapper after codex exits
