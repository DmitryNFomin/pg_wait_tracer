#!/usr/bin/env bash
# check-box-evidence.sh <pr-number>
#
# scripts/git-hooks/pre-push binds LOCAL `make check` evidence to a tree
# hash. Box-check evidence likewise identifies tested bytes by tree-hash=:
# a reconciliation commit can move the PR tip without changing its tree,
# and a log's commit= alone cannot identify the tree actually tested.
# Select by tree, so evidence survives a tree-identical moved commit (#714).
# The newest log for that tree must certify built-from=tree-hash. Historical
# pre-#861 logs outside the selection are ignored, never allowed to poison it.
#
# It also checks a second, unrelated-looking but same-family leak: a PR
# whose branch is named agent/<N>-<slug> (asserting it addresses issue #N,
# possibly SEVERAL -- agent/507-490-v2 names both 507 and 490: every leading
# dash-separated numeric token is parsed, not just the first) but whose
# `closingIssuesReferences` is empty will not close #N on merge.
# `gh pr view --json closingIssuesReferences` is the only authority used for
# this; a "#N" mention in the PR body is NOT a closing reference (and the
# inverse also bites: body text like "should this close #596" DOES close
# #596 on merge -- so body text must never be read either way here).
#
# An empty closing-ref list is CORRECT, not a leak, when #N is already
# CLOSED -- there is nothing left to close, so #N's state is checked via
# `gh issue view` before anything is reported. And even for an OPEN #N, this
# script cannot distinguish "fixes it but forgot the keyword" from
# "deliberately does not close it" (e.g. a PR that says "unblocks #N", not
# "fixes #N") -- so it is reported as a CANDIDATE for a human to read the PR
# body, never asserted as a defect. Measured false-positive rate on live
# data before this distinction existed: 2 of 4 flagged issues were already
# closed (#390, #230).
#
# DESIGN RULE, shared with held.sh/sweep.sh: a failed or indeterminate query
# prints UNKNOWN and exits non-zero. An empty result (no finding) and a
# failed lookup (we don't know) must never look alike -- that exact
# confusion produced a false "free" once and a false "not registered" today.
#
# Output, one or more lines per invocation:
#   EVIDENCE-OK <log-commit> (tree <tree>)     -- a log's tree matches the tip
#   EVIDENCE-TREE-DRIFT tip=<tree> logged=<tree> -- no matching tree; file diff
#      FINDING if src/ or tests/ changed, otherwise NOTE (does not fail)
#   EVIDENCE-UNIDENTIFIED logged tree <tree> unresolvable -- finding
#   EVIDENCE-MISSING                          -- no box-check-*.log found at all
#   NO-CLOSING-REF-CANDIDATE #<pr> -> issue #<N> (open; ...)
#                                              -- branch names open #N with no
#                                                 closing ref; verify the PR
#                                                 body before treating as a bug
#   UNKNOWN: <reason>                         -- query/parse failure
#
# Exit codes:
#   0  no findings (EVIDENCE-OK or NOTE drift, and no closing-ref candidate)
#   1  FINDING drift, EVIDENCE-UNIDENTIFIED, EVIDENCE-MISSING, or a candidate
#   3  UNKNOWN: gh/git query error, unreadable log, unparseable header/field --
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
  mapfile -t LOGS < <(/usr/bin/python3 - "$WT_PATH/tests/results" <<'PYLOGS'
import glob, os, re, sys
def newest_key(path):
    name = os.path.basename(path)
    stamp = re.search(r"(\d{8}-\d{6})\.log$", name)
    return (stamp.group(1) if stamp else name, name)
for path in sorted(glob.glob(os.path.join(sys.argv[1], "box-check-*.log")), key=newest_key):
    if os.path.isfile(path):
        print(path)
PYLOGS
  )
  if [ "${#LOGS[@]}" -eq 0 ]; then
    echo "EVIDENCE-MISSING"
    finding=1
  else
    if ! TIP_TREE=$(git rev-parse "$HEAD_SHA^{tree}" 2>"$TMP/tree.err") || [ -z "$TIP_TREE" ]; then
      echo "UNKNOWN: cannot resolve tip tree for $HEAD_SHA: $(cat "$TMP/tree.err")"
      exit 3
    fi
    legacy_logs=()
    selected=""
    fallback=""
    # Prefer the tip tree regardless of commit=. Within one tree, the newest
    # run decides provenance; otherwise compare the newest available tree.
    for log in "${LOGS[@]}"; do
      if [ ! -r "$log" ]; then
        echo "UNKNOWN: $log is not readable"
        exit 3
      fi
      line=$(/usr/bin/grep -h '^box-check: commit=' "$log" | tail -1)
      if [ -z "$line" ]; then
        line=$(/usr/bin/grep -h '^box-check: source commit=' "$log" | tail -1 | /usr/bin/sed 's/box-check: source /box-check: /')
      fi
      c=$(printf '%s' "$line" | /usr/bin/sed -nE 's/^box-check: commit=([0-9a-f]+)( .*)?$/\1/p')
      if [[ "$line" != *' built-from='* ]]; then
        legacy_logs+=("$log")
      fi
      t=$(printf '%s' "$line" | /usr/bin/sed -nE 's/^box-check: commit=[0-9a-f]+ tree-hash=([0-9a-f]+)([[:space:]].*)?$/\1/p')
      if [ -z "$t" ]; then
        # Malformed certificates remain UNKNOWN. An old legacy header without
        # a usable tree cannot poison a newer, identified certificate.
        if [[ "$line" == *' built-from='* ]]; then
          echo "UNKNOWN: $log has no parseable tree-hash= header"
          exit 3
        fi
        if [ -n "$line" ]; then
          fallback="$log"
          fallback_line="$line"
        fi
        continue
      fi
      fallback="$log" # newest identified tree
      fallback_line="$line"
      [ "$t" = "$TIP_TREE" ] || continue
      selected="$log"
      selected_line="$line"
    done
    if [ -z "$selected" ] && [ -n "$fallback" ]; then
      selected="$fallback"
      selected_line="$fallback_line"
    fi
    for log in "${legacy_logs[@]}"; do
      [ "$log" = "$selected" ] || echo "$log ignored (pre-#861 format)"
    done
    if [ -z "$selected" ]; then
      echo "EVIDENCE-MISSING (no log with an identified tree)"
      finding=1
    else
      log="$selected"
      line="$selected_line"
      c=$(printf '%s' "$line" | /usr/bin/sed -nE 's/^box-check: commit=([0-9a-f]+)( .*)?$/\1/p')
      t=$(printf '%s' "$line" | /usr/bin/sed -nE 's/^box-check: commit=[0-9a-f]+ tree-hash=([0-9a-f]+)([[:space:]].*)?$/\1/p')
      if [ -z "$t" ]; then
        echo "UNKNOWN: $log has no parseable tree-hash= header"
        exit 3
      fi
      b=$(printf '%s' "$line" | /usr/bin/sed -nE 's/^box-check: commit=[0-9a-f]+ tree-hash=[0-9a-f]+ built-from=([0-9a-f]+)([[:space:]].*)?$/\1/p')
      if [ -z "$b" ] || [ "$b" != "$t" ]; then
        echo "UNKNOWN: $log built-from=${b:-missing} differs from tree-hash=$t; rerun box-check"
        exit 3
      fi
      if [ "$t" = "$TIP_TREE" ]; then
        echo "EVIDENCE-OK $c (tree $TIP_TREE)"
      else
        # No matching tree: compare the newest log's tree, as in #714.
        LOGGED_TREE="$t"
        if ! git cat-file -e "$LOGGED_TREE" 2>/dev/null; then
          echo "EVIDENCE-UNIDENTIFIED logged tree $LOGGED_TREE unresolvable (FINDING)"
          finding=1
        else
          if ! git diff-tree -r --name-status "$TIP_TREE" "$LOGGED_TREE" > "$TMP/tree.diff" 2>"$TMP/diff.err"; then
            echo "UNKNOWN: cannot compare tip tree $TIP_TREE to logged tree $LOGGED_TREE: $(cat "$TMP/diff.err")"
            exit 3
          fi
          echo "EVIDENCE-TREE-DRIFT tip=$TIP_TREE logged=$LOGGED_TREE (diff: tip -> logged)"
          if /usr/bin/awk -F'\t' '{ for (i=2; i<=NF; i++) if ($i ~ /^"?(src|tests)\//) found=1 } END { exit !found }' "$TMP/tree.diff"; then
            echo "FINDING: src/ or tests/ changed"
            finding=1
          else
            echo "NOTE: only paths outside src/ and tests/ changed"
          fi
          cat "$TMP/tree.diff"
        fi
      fi
    fi
  fi
fi


# ---- [2] closing-keyword leak -----------------------------------------------
# Only applies when the branch name itself asserts which issue(s) it
# addresses: every leading dash-separated token after "agent/" that is
# PURELY numeric, stopping at the first non-numeric token. This is what
# lets agent/507-490-v2 yield {507, 490} and agent/230-pre-push-hook yield
# just {230} ("pre" stops the scan).
if [[ "$BRANCH" =~ ^agent/(.+)$ ]]; then
  slug="${BASH_REMATCH[1]}"
  IFS='-' read -r -a tokens <<< "$slug"
  nums=()
  for t in "${tokens[@]}"; do
    case "$t" in
      ''|*[!0-9]*) break ;;
      *) nums+=("$t") ;;
    esac
  done
  IFS=',' read -r -a ref_arr <<< "$REFS"
  for N in "${nums[@]}"; do
    covered=0
    for r in "${ref_arr[@]}"; do
      [ "$r" = "$N" ] && { covered=1; break; }
    done
    [ "$covered" -eq 1 ] && continue
    # Not in closingIssuesReferences -- but that is CORRECT if #N is already
    # closed (nothing left to close). Only an OPEN #N with no closing ref is
    # worth a human look, and even then only as a candidate: this script
    # cannot tell "forgot the keyword" from "deliberately does not close it".
    state=$(gh issue view "$N" --json state -q .state 2>"$TMP/issue-$N.err")
    if [ -z "$state" ]; then
      echo "UNKNOWN: could not determine state of issue #$N: $(tr '\n' ' ' < "$TMP/issue-$N.err")"
      exit 3
    fi
    if [ "$state" = "OPEN" ]; then
      echo "NO-CLOSING-REF-CANDIDATE #$PR -> issue #$N (open; verify the PR body -- it may deliberately not close this issue)"
      finding=1
    fi
  done
fi

exit "$finding"
