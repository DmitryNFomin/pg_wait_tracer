#!/usr/bin/env bash
# Draft a PR cost record from wrapper rounds; posting is explicitly opt-in.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
post=0
pr=
for arg in "$@"; do
    case "$arg" in
        --post) post=1 ;;
        *)
            if [[ -n "$pr" || ! "$arg" =~ ^[1-9][0-9]*$ ]]; then
                echo 'usage: cost-record.sh <pr> [--post]' >&2
                exit 2
            fi
            pr=$arg ;;
    esac
done
[[ -n "$pr" ]] || { echo 'usage: cost-record.sh <pr> [--post]' >&2; exit 2; }
head=$(gh pr view "$pr" --json headRefName --jq .headRefName)
branch=${head#agent/}
draft=$(mktemp "${TMPDIR:-/tmp}/pgwt-cost-record.XXXXXX")
trap 'rm -f "$draft"' EXIT
python3 - "${PGWT_LEAD_STATE_DIR:-$here}/costs.tsv" "$branch" "$pr" > "$draft" <<'PY'
import csv
from pathlib import Path
import sys

path, branch, pr = sys.argv[1:]
try:
    with Path(path).open(newline='') as stream:
        rows = [row for row in csv.reader(stream, delimiter='\t')
                if len(row) > 1 and row[1] == branch]
    if not rows:
        raise ValueError(f'no cost rows for branch {branch}')
    if any(len(row) != 8 or not (row[4].isdigit() or row[4] == 'unknown') for row in rows):
        raise ValueError(f'invalid cost row for branch {branch}')
except (OSError, ValueError) as exc:
    sys.exit(f'cost-record: {exc}')

print(f'Cost record: PR #{pr} ({branch})\n')
for number, row in enumerate(rows, 1):
    print(f'- Codex round {number}: {row[4]} tokens; {row[7]} (ended {row[0]}).')
total = sum(int(row[4]) for row in rows if row[4] != 'unknown')
unknown = sum(row[4] == 'unknown' for row in rows)
if unknown:
    print(f'\nCodex total: unknown (known subtotal: {total} tokens; {unknown} rounds unknown).')
else:
    print(f'\nCodex total: {total} tokens ({len(rows)} rounds).')
print('\n- Reviewer tokens: [FILL IN reviewer, rounds, tokens, verdict]')
print('- Adviser tokens: [FILL IN advisers and tokens, or none]')
print('- Gate-box hours: [FILL IN hours]')
PY
cat "$draft"
if [[ "$post" -eq 1 ]]; then
    gh pr comment "$pr" --body-file "$draft"
fi
