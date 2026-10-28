#!/usr/bin/env bash
# check-box-evidence.sh <pr-number>
#
# scripts/git-hooks/pre-push binds LOCAL `make check` evidence to a tree
# hash. There is no equivalent for box-check evidence, and the gap has been
# hit repeatedly: `gh pr update-branch` creates a merge commit with no
# box-check run of its own, so a PR can carry a tests/results/box-check-*.log
# whose commit= header names a different commit than the PR's current tip.
# This script makes that comparison mechanical instead of a manual read.
#
# It also checks a second, unrelated-looking but same-family leak: a PR
# whose branch is named agent/<N>-<slug> (asserting it addresses issue #N)
# but whose `closingIssuesReferences` is empty will not close #N on merge --
# "fixed but left open" has been the single most recurring leak this
# session. `gh pr view --json closingIssuesReferences` is the only
# authority used for this; a "#N" mention in the PR body is NOT a closing
# reference (and the inverse also bites: body text like "should this close
# #596" DOES close #596 on merge -- so body text must never be read either
# way here).
#
# DESIGN RULE, shared with held.sh/sweep.sh: a failed or indeterminate query
# prints UNKNOWN and exits non-zero. An empty result (no finding) and a
# failed lookup (we don't know) must never look alike -- that exact
# confusion produced a false "free" once and a false "not registered" today.
#
# Output, one or more lines per invocation:
#   EVIDENCE-OK <sha>                         -- a log's commit= matches HEAD
#   EVIDENCE-STALE <log-sha> != <head-sha>    -- no log matches HEAD
#   EVIDENCE-MISSING                          -- no box-check-*.log found at all
#   NO-CLOSING-REF #<pr> -> issue #<N>        -- branch names #N, merge won't close it
#   UNKNOWN: <reason>                         -- query/parse failure; nothing else printed
#
# Exit codes:
#   0  everything checked is clean (EVIDENCE-OK and no NO-CLOSING-REF finding)
#   1  a real finding (EVIDENCE-STALE, EVIDENCE-MISSING, or NO-CLOSING-REF)
#   3  UNKNOWN: gh error, unreadable log, or an unparseable header/field --
#      this is deliberately a different code than 1, same reasoning as
#      held.sh's REFUSED: a finding and "we could not tell" must stay
#      distinguishable by more than prose.
#
# This script is STANDALONE and UNWIRED: it is not called from sweep.sh.
# Another branch (agent/lead-dispatcher) is actively rebuilding sweep.sh;
# wiring this in is left to a later, single commit once that lands.
export GH_PAGER=cat; set -u
PR="${1:?usage: check-box-evidence.sh <pr-number>}"
case "$PR" in
  ''|*[!0-9]*) echo "UNKNOWN: '$PR' is not a PR number"; exit 3 ;;
esac
cd /Users/dmitryfomin/work/git/pg_wait_tracer || { echo "UNKNOWN: cannot cd to repo root"; exit 3; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

if ! gh pr view "$PR" --json headRefOid,headRefName,closingIssuesReferences \
      > "$TMP/pr.json" 2>"$TMP/pr.err"; then
  echo "UNKNOWN: gh pr view $PR failed: $(tr '\n' ' ' < "$TMP/pr.err")"
  exit 3
fi
[ -s "$TMP/pr.json" ] || { echo "UNKNOWN: gh pr view $PR returned no data"; exit 3; }

if ! /usr/bin/python3 - "$TMP/pr.json" > "$TMP/pr.fields" 2>"$TMP/pr.pyerr" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    head = d["headRefOid"]
    branch = d["headRefName"]
    refs = d["closingIssuesReferences"]
    if not isinstance(refs, list):
        raise TypeError("closingIssuesReferences is not a list")
    numbers = sorted({str(r["number"]) for r in refs})
except Exception as e:
    print(f"ERROR: {e}", file=sys.stderr)
    sys.exit(1)
print(f"HEAD\t{head}")
print(f"BRANCH\t{branch}")
print(f"REFS\t{','.join(numbers)}")
PY
then
  echo "UNKNOWN: unparseable PR fields: $(cat "$TMP/pr.pyerr")"
  exit 3
fi

HEAD_SHA=$(/usr/bin/awk -F'\t' '$1=="HEAD"{print $2}' "$TMP/pr.fields")
BRANCH=$(/usr/bin/awk -F'\t' '$1=="BRANCH"{print $2}' "$TMP/pr.fields")
REFS=$(/usr/bin/awk -F'\t' '$1=="REFS"{print $2}' "$TMP/pr.fields")
if [ -z "$HEAD_SHA" ] || [ -z "$BRANCH" ]; then
  echo "UNKNOWN: pr view JSON missing headRefOid/headRefName"
  exit 3
fi

finding=0

# ---- [1] box evidence vs HEAD ----------------------------------------------
WT_PATH=$(git worktree list --porcelain 2>/dev/null | /usr/bin/awk -v b="refs/heads/$BRANCH" '
  /^worktree / { path = substr($0, 10) }
  $0 == "branch " b { print path; exit }
')
if [ -z "$WT_PATH" ] || [ ! -d "$WT_PATH/tests/results" ]; then
  echo "EVIDENCE-MISSING"
  finding=1
else
  mapfile -t LOGS < <(find "$WT_PATH/tests/results" -maxdepth 1 -type f \
      -name 'box-check-*.log' 2>/dev/null | sort)
  if [ "${#LOGS[@]}" -eq 0 ]; then
    echo "EVIDENCE-MISSING"
    finding=1
  else
    commits=()
    for log in "${LOGS[@]}"; do
      if [ ! -r "$log" ]; then
        echo "UNKNOWN: $log is not readable"
        exit 3
      fi
      line=$(/usr/bin/grep -h '^box-check: commit=' "$log" | tail -1)
      c=$(printf '%s' "$line" | /usr/bin/sed -nE 's/^box-check: commit=([0-9a-f]+) .*/\1/p')
      if [ -z "$c" ]; then
        echo "UNKNOWN: $log has no parseable commit= header"
        exit 3
      fi
      commits+=("$c")
    done
    match=""
    for c in "${commits[@]}"; do
      [ "$c" = "$HEAD_SHA" ] && { match="$c"; break; }
    done
    if [ -n "$match" ]; then
      echo "EVIDENCE-OK $match"
    else
      # Report the most recent log's commit (logs sorted by filename, which
      # embeds a zero-padded timestamp, so the last element is the newest).
      stale="${commits[${#commits[@]}-1]}"
      echo "EVIDENCE-STALE $stale != $HEAD_SHA"
      finding=1
    fi
  fi
fi

# ---- [2] closing-keyword leak -----------------------------------------------
# Only applies when the branch name itself asserts which issue it closes.
if [[ "$BRANCH" =~ ^agent/([0-9]+)- ]]; then
  N="${BASH_REMATCH[1]}"
  IFS=',' read -r -a ref_arr <<< "$REFS"
  covered=0
  for r in "${ref_arr[@]}"; do
    [ "$r" = "$N" ] && { covered=1; break; }
  done
  if [ "$covered" -eq 0 ]; then
    echo "NO-CLOSING-REF #$PR -> issue #$N"
    finding=1
  fi
fi

exit "$finding"
