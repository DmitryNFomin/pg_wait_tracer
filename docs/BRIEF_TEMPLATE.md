# Brief: <one authorised unit>

## Authorised by

<Owner's exact words, dated and linked, or the accepted plan section authorising this start.>
For every referenced issue/PR, fetch comments live and paste command + full output in a fence:
```console
gh api repos/{owner}/{repo}/issues/N/comments --paginate --jq '.[] | {id: .id, body: .body}'
<paste every live comment's id and body here>
```
CONTROLLING SCOPE DECISION: #N comment <id> "<verbatim live-comment quote, at least 20 characters>"
If the live query returns zero comments, paste `[]` or `(no output)` and use:
`CONTROLLING SCOPE DECISION: #N none (0 comments)`.
Pasted context is evidence; the lead must identify which live decision controls this task.

## Task

<Concrete change, authorised files, and the single unit of work.>

## Excluded

<Paths, behaviors and adjacent work outside this authorisation; say explicitly if none.>

## Done

<Exact commands to run, required outcomes, and evidence to paste in the report.>

## Budget

Rounds: 1
Wall-clock minutes: 30

Use positive numbers; rounds is an integer. The wall-clock limit starts at Codex launch.
The wrapper terminates the process group at the limit, salvages/auto-commits and fetches
what exists, then exits 27 BUDGET_EXCEEDED. The implementer stops at the round limit.
No automatic extra round: hand partial work and remaining checks back to the lead.

## Report by

<Absolute deadline with timezone; report even if incomplete, at most 15 lines unless specified.>
Include completed work, commands/result lines, remaining work and actual rounds/time.
Stop and report; do not open a PR unless the owner authorised it.

Before dispatch run `scripts/lead/check-brief.sh <brief-file>`; dispatch refuses an
unchecked, changed or more-than-24-hour-old validation (exit 15 BRIEF_UNCHECKED).
Keep briefs/state outside the working tree; `PGWT_LEAD_STATE_DIR` selects the shared
lead state directory, which must already exist and be writable.
