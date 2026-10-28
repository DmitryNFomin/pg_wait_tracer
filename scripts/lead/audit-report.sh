#!/usr/bin/env bash
# audit-report.sh <draft-file>
# Runs BEFORE any status report reaches the owner. codex re-derives every
# factual claim from the repo and GitHub and returns VERDICT: SEND or BLOCK.
# The lead does not send a report that comes back BLOCK without fixing it.
set -u
D="${1:?usage: audit-report.sh <draft-file>}"
cd /Users/dmitryfomin/work/git/pg_wait_tracer
H=$(shasum -a 256 "$D" | cut -c1-16)
OUT=$(mktemp); trap 'rm -f "$OUT"' EXIT
codex exec "You are auditing a status report before it reaches the project owner.
The owner is tired of correcting it himself; that is your job now, not his.

Read the draft at $D. For EVERY factual claim in it:
 - counts (blockers, open issues, merged PRs) -> re-derive with gh and compare
 - any claim that work is held/blocked/waiting -> run the query that proves it;
   if the cited file or PR is actually free, that is a BLOCK
 - any claim that something does not exist, is not merged, or is unreachable ->
   verify at a named revision; an unqualified absence is a BLOCK
 - any claim about what is running -> check it is actually running
 - any attribution of the lead's own inaction to an external cause -> BLOCK unless
   the proof is pasted in the draft itself

Be adversarial. The lead's record this week includes: reporting an item 'blocked on
file collisions' for 11 hours after those files were free; saying all GitHub writes
were denied after one refusal; counts wrong by 2-3x; and three absences asserted
without a revision that were false.

Output exactly:
VERDICT: SEND   or   VERDICT: BLOCK
then one line per claim: CLAIM -> VERIFIED <evidence> | WRONG <what it actually is>
Keep it under 20 lines. Do not edit anything." </dev/null > "$OUT" 2>&1
rc=$?
tail -25 "$OUT"
# FAIL CLOSED. A crashed or malformed auditor is a BLOCK, never a pass, and the
# verdict is bound to this draft's hash so an older SEND cannot be reused.
if [ "$rc" -ne 0 ]; then
  printf '\nAUDIT INCONCLUSIVE (auditor exit %s) -- treat as BLOCK. draft=%s\n' "$rc" "$H"; exit 2
fi
if ! /usr/bin/grep -qE '^VERDICT: (SEND|BLOCK)' "$OUT"; then
  printf '\nAUDIT MALFORMED (no VERDICT line) -- treat as BLOCK. draft=%s\n' "$H"; exit 2
fi
if /usr/bin/grep -qE '^VERDICT: BLOCK' "$OUT"; then
  printf '\nVERDICT BLOCK for draft=%s -- do not send.\n' "$H"; exit 1
fi
printf '\nVERDICT SEND for draft=%s -- this hash only.\n' "$H"; exit 0
