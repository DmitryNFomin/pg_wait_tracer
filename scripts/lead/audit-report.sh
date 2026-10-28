#!/usr/bin/env bash
# audit-report.sh <draft-file>
# Runs BEFORE any status report reaches the owner. codex re-derives every
# factual claim from the repo and GitHub and returns VERDICT: SEND or BLOCK.
# The lead does not send a report that comes back BLOCK without fixing it.
set -u
D="${1:?usage: audit-report.sh <draft-file>}"
# Resolve our own directory BEFORE the cd below, and resolve $D to an
# absolute path too -- a relative draft path would otherwise point at the
# wrong tree once we cd into the main checkout.
SELFDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$D" in
  /*) : ;;
  *) D="$(pwd)/$D" ;;
esac
cd /Users/dmitryfomin/work/git/pg_wait_tracer

# ---- deterministic pre-pass, before any codex call -------------------------
# RULE 0: any absence claim ("exists nowhere", "not merged", "unreachable",
# "no mechanical check", "not implemented", ...) needs a pasted command and
# its output. This has been violated routinely -- including reporting checks
# as nonexistent that were written that same morning. Catch the unevidenced
# ones mechanically so a BLOCK does not wait on codex noticing the same
# thing. See absence-prepass.py for the vocabulary and the proximity rule.
if [ ! -r "$D" ]; then
  printf 'PREPASS ERROR: cannot read draft %s -- treat as BLOCK.\n' "$D"
  printf '\nVERDICT: BLOCK\nVERDICT BLOCK for draft=unreadable -- do not send.\n'
  exit 1
fi
H=$(shasum -a 256 "$D" | cut -c1-16)
PP_OUT=$(/usr/bin/python3 "$SELFDIR/absence-prepass.py" "$D" 2>&1)
PP_RC=$?
if [ "$PP_RC" -eq 1 ]; then
  printf '%s\n' "$PP_OUT"
  printf '\nVERDICT: BLOCK\nVERDICT BLOCK for draft=%s -- do not send (pre-pass, no codex call made).\n' "$H"
  exit 1
elif [ "$PP_RC" -ne 0 ]; then
  # Pre-pass itself failed to run (bad interpreter, unreadable file, etc.) --
  # fail closed exactly like a crashed codex auditor would.
  printf '%s\n' "$PP_OUT"
  printf '\nPREPASS INCONCLUSIVE (exit %s) -- treat as BLOCK. draft=%s\n' "$PP_RC" "$H"
  exit 2
fi
printf '%s\n' "$PP_OUT"

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
