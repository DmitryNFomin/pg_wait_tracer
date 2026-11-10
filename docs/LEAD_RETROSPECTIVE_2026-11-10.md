# Lead retrospective, 2026-11-10

Written at the owner's instruction, on the day he stopped new implementation and
asked for a handover. Both advisers reviewed a first draft and found errors in
it, including one that repeats the exact mistake the document is about; those are
corrected here and the ninth instance is recorded in section 3.

## 1. The result, and what the numbers do and do not establish

```
$ gh issue list --label tag-blocker --state open --limit 80 --json number --jq length
20
$ gh issue list --label tag-blocker --state closed --limit 300 --json closedAt --jq '[.[]|select(.closedAt>="2026-11-03")]|length'
18
$ gh issue list --label tag-blocker --state all --limit 300 --json createdAt --jq '[.[]|select(.createdAt>="2026-11-03")]|length'
21
```

Eighteen closed and twenty-one filed over seven days, against twenty open now.
Arithmetic on those three numbers puts the open count seven days ago at
seventeen, but I am labelling that as a derivation and not a measurement: a label
can be added long after an issue is created, an issue can be reopened, and
neither of those shows up in `createdAt`. Reconstructing the true count for a
past day needs the label event history, which I did not pull.

What the queries do support is the direction: more arrived than left. The owner's
reading -- "we still not getting closer" -- is not contradicted by anything I
measured, and the count of open blockers is in any case a poor proxy for
remaining work, since the twenty are not comparable in size.

Two tag-blockers closed today, #818 and #819, as merged PRs #836 and #838. Two
arrived, #837 and #839. I filed #839 myself off a measurement, and it is one of
the more serious defects on the list.

## 2. What limited throughput

**Every implementation round dispatched through `scripts/lead/codex-task.sh` has
ended without a commit, for as long as the logs go back.**

```
$ ls scratchpad/*dispatch*.out | wc -l
      20
$ grep -l "^BRANCH_UNCHANGED" scratchpad/*dispatch*.out | wc -l
      18
$ grep -L "^BRANCH_UNCHANGED" scratchpad/*dispatch*.out | sed "s,.*/,,"
837-measure-dispatch.out
sidecar-measure-dispatch.out
```

The two without that line are not exceptions: both end in
`codex-task.sh: line 101: syntax error`, because I edited the wrapper while they
were running (section 3, item 9). So the honest statement is that twenty of
twenty rounds failed to produce a commit, eighteen of them cleanly.

The cause is one line of sandbox policy: the implementer cannot write under
`.git`, so `git add` cannot take its lock --
`fatal: Unable to create '.../.git/index.lock': Operation not permitted`.

I had been absorbing that by hand, round after round: read the log, extract the
patch, hand it to a landing agent. **The part I own is not the sandbox, it is
that I kept dispatching into a workflow I already knew could not commit, and
treated each salvage as a one-off.** The signal was printed by my own wrapper
every time. Last night three parallel rounds finished with their work reachable
only because the implementers had printed their patches into their logs.

I have since changed the wrapper to write the working tree out as a patch and to
tell the implementer not to spend a round fighting git. **That change is
committed in the same branch as this document; before that commit it existed only
in my local checkout, which means the handover's dispatch recipe did not work for
anyone who cloned the repository.** An adviser caught that, not me.

The second limit was me. The largest single consumer of today's turns was
auditing my own prose: seven audit rounds, two returning BLOCK with eight and
then seven wrong claims. I have not instrumented that into hours, so I am not
giving a figure; the count of rounds is what I can show.

## 3. The failure pattern, nine instances

Every wrong claim caught today was a **derived comparison** -- a ratio, a scale,
a fraction, a generalisation across captures -- and never a measured number. The
measurements were right every time.

1. I wrote that both salvaged patches were backed by a green box run. True of
   #819; #821's box run had been cut off mid live-UI-smoke with no aggregate
   line. I knew that, having briefed its landing agent to re-run it.
2. "Two issues filed in the last twenty-four hours" when one was twenty-six
   hours old.
3. **Published on #381:** a 1,647.92 ms gap was "the same absolute scale" as what
   the issue records. That was an adviser's derivation from percentages; the
   repository contains the measurement file recording 4,698.996 ms and
   3,698.999 ms. I had a standing note that adviser numbers need the provenance I
   demand of my own, and I passed it through regardless.
4. **Published on #381, in the correction to (3):** "roughly a third", where
   1647.922905/3698.999 = 0.4455, about 45 percent.
5. That #821 deletes the gate rule mapping #410's fields. It deletes #818's rule;
   #410's mappings survive.
6. That the implementer sandbox was the cause of all three failed rounds; #802's
   round was a diagnosis with no product patch by design.
7. That both dispatches I broke "died". One died; the other finished its work
   first.
8. That `chmod 000` was "the only route the daemon can produce". A missing or
   partly written sidecar is reachable too.
9. **Published on #381, and found by an adviser reviewing the first draft of this
   very document:** that #408's measurement "was already aligned and still
   records DB +4698.996 ms", making it a counterexample to the
   window-alignment explanation. That file's third row is
   `=== full span (1929 s) ===  delta DB +0.000 (+0.000%)  Idle +0.000 (+0.000%)`.
   #408's full span is zero, exactly like today's; its +4,698.996 ms is a
   900-second sub-window, which is precisely where boundary placement applies. So
   #408 supports the explanation I said it contradicted -- and both (3) and (9)
   came out of the same file, whose rows I had still not read after being
   corrected once.

Three of those nine were published where the owner can read them, on #381, and
all three are corrected there with the originals left visible.

**On "what I have changed": instance (9) happened after I wrote, in the first
draft of this document, that I had stopped publishing derived comparisons.** So I
am recording an intent and a mechanism rather than a change:

- publish the measurement and its command, and let the reader divide;
- before citing a measurement file, read all of it, and paste the rows I rely on;
- hold an adviser's number to the evidence bar I apply to my own;
- say "on this capture" until a second capture agrees.

Only the first of those has ever survived a day, and today it did not. The
honest version is that the next person should not take my word for a derived
number, and neither should I.

## 4. What I would do differently, in order of expected value

1. **Fix the tool the second time you hand-salvage anything.** The signal was
   available twenty rounds ago in a line my own wrapper printed.
2. **Never edit a script while an instance of it is running.** `sh` reads a
   script by byte offset, so a running wrapper resumes into shifted bytes.
   `sh -n` passed and the lead selftest passed: the file was fine, the running
   readers were not. One dispatch lost its fetch-and-salvage step; its result
   survived only in its log.
3. **Audit the brief, not just the report.** The audits caught real errors, but
   they caught them downstream, in prose. The same scrutiny spent on a brief
   before dispatch is cheaper and catches the measurement design.
4. **A code read is not a defect report.** Both measurement rounds overturned a
   code-read conclusion. #839 exists because an adviser refused to let me file a
   code read, and the measurement then found the defect was worse than the read
   suggested and that my reachability claim was too narrow. #837's severity and
   #381's cause were likewise unknowable from `grep`.
5. **Treat the arrival rate as part of the plan.** #837 and #839 both came out of
   our own measurement rounds this week. That is the system working -- they are
   real defects -- but it means the open count is a discovery process, not a
   burn-down, and nobody should hand the owner a date derived from it.
