#!/usr/bin/env bash
# audit-report.sh <draft-file>
# Runs BEFORE any status report reaches the owner. codex re-derives every
# factual claim from the repo and GitHub and returns VERDICT: SEND or BLOCK.
# The lead does not send a report that comes back BLOCK without fixing it.
set -uo pipefail
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
H=$(shasum -a 256 "$D" | cut -c1-16) || {
  printf '\nAUDIT INCONCLUSIVE (cannot hash draft) -- treat as BLOCK.\n'
  exit 2
}
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

OUT="$D.audit-$H.txt"
printf 'Auditor output: %s\n' "$OUT"
# Truncate on every invocation: an earlier SEND for the same draft is not a
# substitute for a successful audit now. Keep stdout AND stderr for inspection.
if ! : > "$OUT"; then
  printf '\nAUDIT INCONCLUSIVE (cannot save auditor output) -- treat as BLOCK. draft=%s\n' "$H"
  exit 2
fi
codex exec "You are auditing a status report before it reaches the project owner.
The owner is tired of correcting it himself; that is your job now, not his.

Read the draft at $D. For EVERY factual claim in it:
 - counts (blockers, open issues, merged PRs) -> re-derive with gh and compare
 - any claim that work is held/blocked/waiting -> run the query that proves it;
   if the cited file or PR is actually free, that is a BLOCK
 - any claim that something does not exist, is not merged, or is unreachable ->
   verify at a named revision; an unqualified absence is a BLOCK
 - any claim about what is running -> check it is actually running
 - any attribution of the lead's own inaction to an external cause -> check the
   pasted proof; if the relevant session is inaccessible, mark UNVERIFIABLE

Be adversarial. The lead's record this week includes: reporting an item 'blocked on
file collisions' for 11 hours after those files were free; saying all GitHub writes
were denied after one refusal; counts wrong by 2-3x; and three absences asserted
without a revision that were false.

Use one line per claim: CLAIM -> VERIFIED <evidence>, CLAIM -> WRONG <contradicting
evidence>, or CLAIM -> UNVERIFIABLE <why access is unavailable>.
WRONG means you checked and evidence contradicts the claim. UNVERIFIABLE is only
for lack of access, never for 'I did not look': attempt available checks first.
Access failures include HTTP 401/403 from gh, a missing API token (including hcloud),
the lead's own session (such as a permission denial or a hook firing) that you
cannot observe, and a required process listing you cannot obtain. These are
UNVERIFIABLE, not WRONG. Report them separately; they alone do not require BLOCK.
Do not hide a checked, contradicted claim behind UNVERIFIABLE.
Finish with exactly one standalone verdict line, either 'VERDICT: SEND' or
'VERDICT: BLOCK'. Any WRONG requires BLOCK; lack of access alone permits SEND.
Keep it under 20 lines. Do not edit anything." </dev/null > "$OUT" 2>&1
rc=$?
tail -25 "$OUT"
# Misclassifying a real error as UNVERIFIABLE lets a bad claim through: that is
# the dangerous direction. The prompt restricts it to lack of access, not effort.
# Only whole verdict lines count, so an echoed prompt offering both choices is
# not a verdict. Remember the last line and detect conflicting distinct values.
STATS=$(/usr/bin/awk '
  /^VERDICT: (SEND|BLOCK)[[:space:]]*$/ {
    pvn++; pverd[pvn] = $2; p_last_line = NR
  }
  /^(VERIFIED|WRONG|UNVERIFIABLE)([[:space:]]|$)/ ||
  / -> (VERIFIED|WRONG|UNVERIFIABLE)([[:space:]]|$)/ {
    match($0, /(^| -> )(VERIFIED|WRONG|UNVERIFIABLE)([[:space:]]|$)/)
    status = substr($0, RSTART, RLENGTH)
    sub(/^ -> /, "", status); sub(/[[:space:]].*$/, "", status)
    pcount[status]++
  }
  /^codex[[:space:]]*\r?$/ {
    if (prev == "") { pvn = 0; p_last_line = 0; delete pverd; delete pcount }
  }
  { prev = $0 }
  END {
    if (pvn == 0) {
      printf "%d %d %d NONE 0 0\n", pcount["VERIFIED"]+0, pcount["WRONG"]+0,
        pcount["UNVERIFIABLE"]+0
      exit
    }
    last_val = pverd[pvn]
    distinct_seen[last_val] = 1; distinct = 1
    for (i = 1; i <= pvn; i++)
      if (!(pverd[i] in distinct_seen)) { distinct_seen[pverd[i]] = 1; distinct++ }
    printf "%d %d %d %s %d %d\n", pcount["VERIFIED"]+0, pcount["WRONG"]+0,
      pcount["UNVERIFIABLE"]+0, last_val, p_last_line, distinct
  }
' "$OUT") || {
  printf '\nAUDIT MALFORMED (cannot parse output) -- treat as BLOCK. draft=%s\n' "$H"
  exit 2
}
read -r VERIFIED WRONG UNVERIFIABLE VERDICT VERDICT_LINE DISTINCT <<< "$STATS"
printf '\n%s verified, %s wrong, %s unverifiable\n' "$VERIFIED" "$WRONG" "$UNVERIFIABLE"
if [ "$VERDICT" != NONE ]; then
  printf 'Last verdict: %s (line %s of %s)\n' "$VERDICT" "$VERDICT_LINE" "$OUT"
fi
# FAIL CLOSED. A crashed or malformed auditor is a BLOCK, never a pass, and the
# verdict is bound to this draft's hash so an older SEND cannot be reused.
if [ "$rc" -ne 0 ]; then
  printf '\nAUDIT INCONCLUSIVE (auditor exit %s) -- treat as BLOCK. draft=%s\n' "$rc" "$H"; exit 2
fi
if [ "$VERDICT" = NONE ]; then
  printf '\nAUDIT MALFORMED (no VERDICT line) -- treat as BLOCK. draft=%s\n' "$H"; exit 2
fi
if [ "$DISTINCT" -gt 1 ]; then
  printf '\nAUDIT AMBIGUOUS (conflicting SEND and BLOCK verdicts) -- treat as BLOCK. draft=%s\n' "$H"; exit 1
fi
NOW=$(shasum -a 256 "$D" | cut -c1-16) || {
  printf '\nAUDIT INCONCLUSIVE (cannot rehash draft) -- treat as BLOCK. draft=%s\n' "$H"
  exit 2
}
if [ "$NOW" != "$H" ]; then
  printf '\nAUDIT INCONCLUSIVE (draft changed during audit) -- treat as BLOCK. draft=%s\n' "$H"; exit 2
fi
if [ "$VERDICT" = BLOCK ] || [ "$WRONG" -gt 0 ]; then
  printf '\nVERDICT BLOCK for draft=%s -- do not send.\n' "$H"; exit 1
fi
printf '\nVERDICT SEND for draft=%s -- this hash only.\n' "$H"; exit 0
