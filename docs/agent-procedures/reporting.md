# Reporting and evidence

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

## Absence scopes and probes (from PR #799)

Every absence claim also needs, within 8 lines, a `SEARCHED: <what was probed>` line
and a `NOT SEARCHED: <what could hold it and was not probed>` line. Values must be
at least eight characters and non-vacuous: `none`, empty, `n/a`, or claims to have
searched everything are refused. `UNVERIFIED:` does not stand in for another
claim's evidence (`scripts/lead/absence-prepass.py`).

A Bash call may contain probes or conclusions, never both. Where a verdict is
wanted, derive it in the shell from the data (`out=$(...); if [ -z "$out" ]; then
echo "VERDICT: ..."; fi`), never in an `echo` written alongside the probe. Never pipe
an existence probe to `wc -l` or `head`. A ref, file or process existing is not
evidence of what created it — get its date and author.
