#!/usr/bin/env bash
# held.sh <path>...  -- the ONLY sanctioned source of a "held" claim.
# Fails LOUDLY rather than printing FREE when it cannot establish the answer:
# a silent FREE is the failure this script exists to prevent.
export GH_PAGER=cat; set -u
cd /Users/dmitryfomin/work/git/pg_wait_tracer
prs=$(gh pr list --state open --limit 100 --json number -q '.[].number' 2>&1) || {
  echo "REFUSED: gh pr list failed -- no FREE/HELD claim may be made"; exit 3; }
[ -n "$prs" ] || { echo "REFUSED: gh returned zero open PRs; that is an auth/API failure, not an empty repo"; exit 3; }
tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
n_pr=0
for n in $prs; do                      # NO number filter: #33/#34 hold src/server.c
  f=$(gh pr view "$n" --json files -q '.files[].path' 2>/dev/null)
  [ -n "$f" ] || f=$(gh pr view "$n" --json files -q '.files[].path' 2>/dev/null)
  [ -n "$f" ] || { echo "REFUSED: could not read files for PR #$n"; exit 3; }
  printf '%s\n' "$f" | sed "s|^|$n |" >> "$tmp"; n_pr=$((n_pr+1))
done
echo "# scanned $n_pr open PRs ($(date -u +%H:%M:%SZ))"
for p in "$@"; do
  hits=$(/usr/bin/awk -v P="$p" '$2==P {printf " #%s", $1}' "$tmp")
  if [ -n "$hits" ]; then printf 'HELD %s by%s\n' "$p" "$hits"
  else printf 'FREE %s\n' "$p"; fi
done
